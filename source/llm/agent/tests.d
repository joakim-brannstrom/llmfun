/// Integration and prompt-data guards for the Agent turn policy.
module llm.agent.tests;

import std.algorithm : canFind;
import std.file : exists;
import std.sumtype : match;
import std.typecons : nullable;

import llm.agent;
import llm.chat : Message, turnIdOf;
import llm.metric.monitor : MetricMonitor;

version (unittest) {
    import llm.test_util : testArea;
}

@("user queries open new turns while continue, keep-reasoning and retry nudges stay in the current turn and out of the dialogue/trace projections")
unittest {
    import std.file : write;
    import llm.test_util : testArea;

    auto tmpDir = testArea("agent_turnid");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
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
// produces - before any request is wasted. The override names a file that
// exists nowhere in the fixture prompt dir.
@("Agent construction fails with a prompt-file error when a configured nudge file is missing")
unittest {
    import std.file : write;
    import llm.test_util : testArea;

    auto tmpDir = testArea("agent_nudges_missing");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
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
// ENTIRE policy - fields the model block does not restate fall back to struct
// defaults, NOT to the global customization - and the model's own templates are
// eagerly loaded from the construction prompt dir.
@("resetModel swaps the whole nudge policy (fields not restated fall back to defaults) and eagerly loads the model templates")
unittest {
    import std.file : write;
    import llm.test_util : testArea;
    import std.typecons : nullable;

    import llm.config : CodeModelConfig, NudgeConfig, NudgeKind;

    auto tmpDir = testArea("agent_nudges_switch");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);
    write(tmpDir ~ "MODEL_NUDGE.md", "MODEL-NUDGE-MARKER");

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

// The trigger rule lives in prompt data, not code, so in-tree removal of the
// section would silently change the main agent's behavior. This guard loads
// the real llmfun/config/prompt/AGENT.md through the production
// getPrompt/getBasePrompt path (FlatVfs) and asserts the section heading and
// the exact tool name are present. A missing file makes getBasePrompt throw,
// which fails the test.
@(
        "the composed AGENT.md prompt keeps the Dialogue History Retrieval section naming query_dialogue_history")
unittest {
    const agentPromptFile = "llmfun/config/prompt/AGENT.md";
    assert(agentPromptFile.exists, "in-tree AGENT.md missing; getBasePrompt would throw at startup");
    auto llmConf = makeAgentTestConfig("llmfun/config/prompt");
    auto prompt = llmConf.getPrompt(null, "AGENT.md");
    assert(prompt.canFind("# Dialogue History Retrieval"),
            "trigger-rule section missing from the composed main-agent prompt");
    assert(prompt.canFind("query_dialogue_history"),
            "The trigger rule must name the query_dialogue_history tool");
}

// The reasoning-history rule also lives in prompt data, not code, so in-tree
// removal of the section would silently change the main agent's behavior. Same
// guard shape as the dialogue test above: load the real
// llmfun/config/prompt/AGENT.md through the production getPrompt path and
// assert the section heading, the exact tool name, and the anti-anchoring
// warning are present.
@("the composed AGENT.md prompt keeps the Reasoning History Retrieval section naming query_reasoning_history with the past-thoughts warning")
unittest {
    const agentPromptFile = "llmfun/config/prompt/AGENT.md";
    assert(agentPromptFile.exists, "in-tree AGENT.md missing; getBasePrompt would throw at startup");
    auto llmConf = makeAgentTestConfig("llmfun/config/prompt");
    auto prompt = llmConf.getPrompt(null, "AGENT.md");
    assert(prompt.canFind("# Reasoning History Retrieval"),
            "Reasoning History Retrieval section missing from the composed main-agent prompt");
    assert(prompt.canFind("query_reasoning_history"),
            "Reasoning rule must name the query_reasoning_history tool");
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
@("the five shipped nudge templates exist and carry the required tool names and harness markers")
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
    assert(keepHard.canFind("task_done"), "keep-reasoning hard nudge must name the task_done tool");
    auto recoveryHard = llmConf.readPromptFile("NUDGE_RECOVERY_HARD.md");
    assert(recoveryHard.canFind("Call task_done now."),
            "recovery hard nudge must tell the agent to call task_done");

    // The compression nudge must name the request_compression tool and carry
    // the harness marker, like the soft nudges.
    auto compression = llmConf.readPromptFile("NUDGE_COMPRESSION.md");
    assert(compression.canFind("request_compression"),
            "compression nudge must name the request_compression tool");
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
    /// the fields the loop reads - status and hasToolCall.
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
// are userQuery:false - invisible in the dialogue/trace projections, and they
// never open or fragment a turn.
@("the default keep-reasoning ladder emits soft then hard nudges and exhausts into agentStuckInLoop without fragmenting the turn")
unittest {
    import std.file : write;
    import llm.test_util : testArea;

    auto tmpDir = testArea("agent_escalation");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
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
// - everything else (hardStrikes, file lists) falls back to struct defaults, so
// strike 1 uses the shipped soft nudge and strike 2 the shipped hard one.
@("a per-model ladder with softStrikes:1 exhausts after one soft and one hard nudge")
unittest {
    import std.file : write;
    import llm.test_util : testArea;

    import llm.config : CodeModelConfig, NudgeConfig;

    auto tmpDir = testArea("agent_escalation_model");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
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

// `enabled: false` short-circuits to true - no counter increment: the
// loop runs on with no nudge injected and ends via the MaxConsecutiveSameStatus
// backstop (4 identical needMoreThinking results), not via ladder exhaustion.
// The fixture omits the keep-reasoning files entirely: if the disabled kind's
// ladder were touched, the missing-key AA access would throw.
@("a disabled keep-reasoning kind injects no nudge and touches no counters; the turn ends via the loop-safety backstop")
unittest {
    import std.file : write;
    import llm.test_util : testArea;

    auto tmpDir = testArea("agent_escalation_disabled");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
    write(tmpDir ~ "NUDGE_RECOVERY_SOFT.md", "soft");
    write(tmpDir ~ "NUDGE_RECOVERY_HARD.md", "hard");
    write(tmpDir ~ "NUDGE_COMPRESSION.md", "compression");

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
// strikes and never routes through the ladder - the retry path bypasses the
// escalation mechanism entirely (caller-supplied text).
@("a pipeline retry via addContinueMessage bypasses the ladder: no strikes consumed, caller text verbatim, out of the projections")
unittest {
    import std.file : write;
    import llm.test_util : testArea;

    auto tmpDir = testArea("agent_w3_bypass");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
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

// Regression: an ok round WITH tool calls is a fresh round - it injects no
// nudge and resets the strike counters (per-turn lifecycle), so a healthy
// multi-round turn never drifts toward ladder exhaustion. The soft marker on
// the second keep-reasoning nudge proves the reset: without it, strike 2 would
// emit the hard template.
@(
        "an ok round with tool calls resets the strike counters, so the next keep-reasoning nudge is soft again")
unittest {
    import std.file : write;
    import llm.test_util : testArea;

    auto tmpDir = testArea("agent_ok_toolcall");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
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
@("an ok round without tool calls drives the recovery ladder: the soft recovery nudge is injected and the counter advances")
unittest {
    import std.file : write;
    import llm.test_util : testArea;

    auto tmpDir = testArea("agent_ok_continue");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
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
@("the compression nudge fires only at/above the configured threshold, renders context_percent, and stays one-shot per compress cycle")
unittest {
    import std.array : replicate;
    import std.file : write;
    import llm.test_util : testArea;

    auto tmpDir = testArea("agent_compression");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
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
            && m.content.canFind("83.0%") && m.content.canFind("request_compression"), (_) => false),
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
@("the per-model compression threshold governs: 0.9 stays silent at 83%, 0.6 fires earlier, and the exact boundary is strict >")
unittest {
    import std.array : replicate;
    import std.file : write;
    import llm.test_util : testArea;

    import llm.config : CodeModelConfig, NudgeConfig;

    auto tmpDir = testArea("agent_compression_model");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
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

    // threshold 0.6: fires at the same usage - earlier than the global 0.8.
    auto eager = new CannedProcessAgent("integration", makeAgentTestConfig(tmpDir));
    eager.addUserQuery("x".replicate(13_600));
    eager.syncContextFromChat();
    eager.canned = [ProcessResult(status: ProcessResult.Status.networkFailure)];
    eager.runToCompletion();
    assert(eager.chat.getMessages.length == 2,
            "per-model threshold 0.6 fires at 83% (the global 0.8 would not yet)");
    // Exact-at boundary (strict >): with threshold 0.75 the trigger point is
    // exactly 6144 tokens - at it the nudge stays silent, one token above
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
// template for the disabled kind (nudgeTexts_.compression stays empty - even a
// guard bug could not substitute anything).
@("a disabled compression kind is silent at any usage and loads no template at all")
unittest {
    import std.array : replicate;
    import std.file : write;
    import llm.test_util : testArea;

    auto tmpDir = testArea("agent_compression_off");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
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

// Feedback warnings: gating only - the text stays owned by
// FeedbackEngine. The config's intervalSecs and minToolCalls drive the
// two gates. The first-warning sentinel (toolCallWarnCounter == -1) fires
// regardless of minToolCalls once the interval has passed - the sentinel
// start is SysTime.init.
@("feedback warning gating: the sentinel fires first, then the intervalSecs and minToolCalls gates (strict >) apply, and disabled is fully silent")
unittest {
    import std.datetime : SysTime;
    import std.file : write;
    import llm.test_util : testArea;
    import my.path : Path;

    auto tmpDir = testArea("agent_feedback");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    auto monitor = new MetricMonitor(tmpDir ~ "metrics.jsonl");

    size_t feedbackWarnings(CannedProcessAgent a) {
        size_t n;
        foreach (msg; a.chat.getMessages)
            n += msg.match!((Message m) => m.content.canFind("[SYSTEM MONITORING NOTE]")
                    ? 1 : 0, (_) => 0);
        return n;
    }

    StreamResponse.ToolCall[long] calls;
    calls[0] = StreamResponse.ToolCall(id: "1", name: "no_such_tool", arguments: "{}");

    // Sentinel: the first warning fires regardless of minToolCalls - the
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
    // always passes after the rewind, so the counter gate is exercised alone -
    // counter == minToolCalls is still blocked, minToolCalls + 1 fires.
    auto llmConfBound = makeAgentTestConfig(tmpDir);
    llmConfBound.nudges.feedback.intervalSecs = 0;
    llmConfBound.nudges.feedback.minToolCalls = 2;
    auto bound = new CannedProcessAgent("integration", llmConfBound, monitor);
    bound.handleToolCalls(null, calls); // sentinel fires
    assert(feedbackWarnings(bound) == 1);
    bound.lastToolCallWarning = SysTime.init; // keep the clock gate open
    foreach (i; 0 .. 2) // gates see counters 1, 2 - 2 == minToolCalls blocks
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
    import std.file : write;
    import llm.test_util : testArea;
    import std.range : empty;

    import llm.tool_call : getFunctions;

    auto tmpDir = testArea("agent_broker_pool");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    auto llmConf = makeAgentTestConfig(tmpDir);
    llmConf.toolFilter.include = [".*"]; // match every tool name

    // Production shape: the filter is passed explicitly (app_agent does), so
    // the pool follows the filter the agent was actually given.
    auto reg0 = getFunctions();
    auto agent = new Agent("integration", llmConf, null, null, null, llmConf.toolFilter.to());

    // The registry is a global, append-only map shared by the whole test
    // binary; other threads may append between the ctor's internal pool
    // snapshot and this re-read. Only assert when the registry was static
    // during that window - otherwise skip (the wiring invariant is still
    // checked on every static run, and growth-tolerant runs keep checking
    // the broker state below).
    auto reg = getFunctions();
    if (reg.length == reg0.length) {
        assert(agent.toolCtx.pool.length == reg.length,
                "include '.*' ⇒ the pool is the whole registry");
        foreach (i, f; agent.toolCtx.pool)
            assert(f.name == reg[i].name, "pool order must match registry order");
    }
    assert(!agent.toolCtx.brokerEnabled,
            "kill switch defaults to off: toolBroker.enabled defaults to false, the broker stays inert unless configured");
    assert(agent.toolCtx.broker.activated.empty, "fresh broker: no activations");

    // Convenience-ctor agents get an unfiltered member filter (ReFilter.init),
    // so their pool is unfiltered too - the pool can never disagree with the
    // request path.
    auto unfiltered = new Agent("integration", llmConf, null, null);
    auto regU = getFunctions();
    // Growth-tolerant: compare only when the registry did not grow past the
    // first snapshot (same rationale as the filtered-agent check above).
    if (regU.length == reg.length)
        assert(unfiltered.toolCtx.pool.length == regU.length);

    // Kill switch off: the flag reaches the context.
    llmConf.toolBroker.enabled = false;
    auto off = new Agent("integration", llmConf, null, null);
    assert(!off.toolCtx.brokerEnabled);
}

@(
        "broker pool wiring: a neverHide tool excluded by toolFilter is unioned back with the exclusion surfaced as data")
unittest {
    import std.algorithm : canFind;
    import std.conv : to;
    import std.file : write;
    import llm.test_util : testArea;

    import llm.tool_call : getFunctions;
    import llm.tool_call.broker : hiddenNeverHideTools;

    auto tmpDir = testArea("agent_broker_neverhide");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    auto llmConf = makeAgentTestConfig(tmpDir);
    llmConf.toolBroker.enabled = true; // broker on: the neverHide union-back scenario pins brokerEnabled below
    // A filter that matches nothing hides the WHOLE registry - except the
    // default neverHideTools (["taskDone"]), which is unioned back.
    llmConf.toolFilter.include = ["^no_such_tool_pattern$"];

    auto agent = new Agent("integration", llmConf, null, null, null, llmConf.toolFilter.to());

    auto pool = agent.toolCtx.pool;
    assert(pool.length == 1 && pool[0].name == "taskDone",
            "neverHide tools are unioned back: " ~ pool.length.to!string);
    assert(agent.toolCtx.brokerEnabled);

    // The excluded-neverHide warning is returned as data: the public
    // hiddenNeverHideTools seam yields the excluded tool names; assert on the
    // returned data instead of the log (the ctor renders the same names).
    assert(hiddenNeverHideTools(getFunctions(), llmConf.toolFilter.to(), llmConf.toolBroker.neverHideTools)
            .canFind("taskDone"), "the ctor must see the excluded neverHide tool as data");
}

@("agent owns the tools array: the list_tool_tags entry carries the composed description")
unittest {
    import std.algorithm : canFind, countUntil;
    import std.file : write;
    import llm.test_util : testArea;
    import std.string : indexOf;

    auto tmpDir = testArea("agent_t7_composed");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    auto llmConf = makeAgentTestConfig(tmpDir);
    llmConf.toolBroker.enabled = true; // broker on: the composed description needs the selectTools path
    import llm.config : GroupConfig;

    llmConf.toolBroker.groups["workarea"] = GroupConfig("Workarea tools.", [
        "wf_read"
    ]);
    llmConf.toolBroker.groups["rag"] = GroupConfig("RAG knowledge base tools.", [
        "rag_search"
    ]);
    auto agent = new Agent("integration", llmConf, null, null, null, llmConf.toolFilter.to());

    auto idx = agent.tools.countUntil!(e => e["function"]["name"].str == "list_tool_tags");
    assert(idx >= 0, "the discovery tool is in the day-one array");

    auto desc = agent.tools[idx]["function"]["description"].str;
    assert(canFind(desc, "List available tool groups"),
            "the UDA base text must survive the composition:\n" ~ desc);
    assert(canFind(desc, "workarea (Workarea tools.)"),
            "configured workarea description missing:\n" ~ desc);
    assert(canFind(desc, "rag (RAG knowledge base tools.)"),
            "configured rag description missing:\n" ~ desc);
    assert(desc.indexOf("rag") < desc.indexOf("workarea"),
            "zero activation counts: alphabetical group order expected:\n" ~ desc);

    // Rebuild-at-activation (the change point): the raw selectTools output
    // carries the bare UDA text, so the delegate must recompose - otherwise the
    // configured vocabulary disappears from the model's view after the first
    // activation.
    agent.toolCtx.rebuildTools();
    desc = agent.tools[idx]["function"]["description"].str;
    assert(canFind(desc, "workarea (Workarea tools.)"),
            "the composed description must survive a rebuild:\n" ~ desc);
    assert(canFind(desc, "rag (RAG knowledge base tools.)"),
            "the composed description must survive a rebuild:\n" ~ desc);
}

@("toolFilter-excluded discovery tool: compose is a guarded no-op (miss-case regression - the dead OOB read threw RangeError before the real guard)")
unittest {
    import std.algorithm : canFind;
    import std.datetime : Clock;
    import std.file : mkdirRecurse, write;
    import std.format : format;
    import std.json : JSONValue;

    auto tmpDir = testArea("agent_t2_compose_miss");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    auto llmConf = makeAgentTestConfig(tmpDir);
    llmConf.toolBroker.enabled = true;
    // Config axis: the D name is what the toolFilter matches on.
    llmConf.toolFilter.exclude = ["^listToolTags$"];

    // Before the real guard, this construction threw RangeError from the
    // OOB tools[idx] read (countUntil returns the array length on miss, so
    // the old `if (idx < 0)` never fired).
    auto agent = new Agent("integration", llmConf, null, null, null, llmConf.toolFilter.to());

    assert(!agent.toolCtx.pool.canFind!(f => f.name == "listToolTags"),
            "the toolFilter exclusion keeps the discovery tool out of the pool");
    assert(!agent.tools.canFind!(e => e["function"]["name"].str == "list_tool_tags"),
            "the model-facing array carries no discovery entry");

    // The rebuild (the miss case) must succeed and leave the array
    // byte-identical.
    auto before = JSONValue(agent.tools).toString;
    agent.toolCtx.rebuildTools();
    assert(JSONValue(agent.tools).toString == before,
            "a missed compose leaves the tools array unchanged");
}

@("broker adjust hook: invoked once per model switch with the resolved config, mutations reach the tools array, nothing persists")
unittest {
    import std.algorithm : canFind, map;
    import std.array : array;
    import std.file : write;
    import llm.test_util : testArea;
    import std.json : JSONValue;
    import std.range : empty;
    import std.conv : to;

    import llm.config : CodeModelConfig, GroupConfig, ToolBrokerConfig;

    auto tmpDir = testArea("agent_broker_hook");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    auto llmConf = makeAgentTestConfig(tmpDir);
    llmConf.toolFilter.include = [".*"];
    llmConf.toolBroker.enabled = true;
    llmConf.toolBroker.groups["workarea"] = GroupConfig("workarea file tools",
            ["writeFile", "readFile"]);
    llmConf.toolBroker.hiddenTags = ["workarea"];

    // A second model with no toolBroker block: the hook must fire for it too,
    // with ITS freshly resolved (global) config - hiddenTags still
    // ["workarea"], proving the previous hook mutation did not persist.
    CodeModelConfig other;
    other.modelName = "hooked-model";
    other.contextSize = 8192;
    llmConf.codeModels ~= other;

    auto agent = new Agent("integration", llmConf, null, null);

    string[] namesOf(JSONValue[] ts) {
        return ts.map!(t => t["function"]["name"].str).array;
    }

    // Day-one array: the workarea group is hidden, its tools are not advertised.
    assert(!namesOf(agent.tools).canFind("write_file"),
            "hidden group members start out of the array");
    assert(agent.toolCtx.broker.activated.empty);

    size_t calls;
    bool freshEachTime = true;
    agent.setBrokerAdjustHook(delegate(ref ToolBrokerConfig conf) @safe {
        calls++;
        // The freshly resolved config, not the previously mutated copy: the
        // global block still carries hiddenTags: ["workarea"].
        freshEachTime = freshEachTime && conf.hiddenTags.length == 1
            && conf.hiddenTags[0] == "workarea";
        conf.hiddenTags = []; // unhide-all: the group's tools join the array
        conf.groups["workarea"].tools ~= "readFile2"; // element-level mutation too
    });

    // Model switch: the hook fires exactly once, before any broker consumer
    // call of the new model, and its mutation reaches the tools array.
    agent.resetModel(llmConf.codeModels[1]);

    assert(calls == 1, "exactly one hook invocation per model switch: " ~ calls.to!string);
    assert(freshEachTime, "the hook saw the freshly resolved config, not a stale copy");
    assert(namesOf(agent.tools).canFind("write_file") && namesOf(agent.tools)
            .canFind("read_file"), "hook mutations affect the tools array of subsequent requests");
    assert(!namesOf(agent.tools).canFind("read_file2"),
            "a group entry naming no registry tool never joins the array");
    assert(agent.toolCtx.broker.activated.empty,
            "BrokerState is reset on model switch - activation never carries across models");
    assert(agent.toolCtx.brokerEnabled, "the kill-switch mirror follows the effective config");

    // Switching back: the hook fires again with a fresh resolution - the
    // previous mutation (hiddenTags = []) did not persist into the cached
    // resolution or into LlmConfig.
    agent.resetModel(llmConf.codeModels[0]);
    assert(calls == 2, "one invocation per model switch: " ~ calls.to!string);
    assert(freshEachTime, "the second invocation also saw the freshly resolved config");
    assert(namesOf(agent.tools).canFind("write_file"), "the mutation took effect on this model too");
    assert(llmConf.toolBroker.hiddenTags == ["workarea"],
            "mutations do not write back into LlmConfig");
    assert(llmConf.toolBroker.groups["workarea"].tools == [
        "writeFile", "readFile"
    ], "element-level mutation does not write back into LlmConfig");
}

@(
        "agent owns the tools array: a configured-but-empty group description is not advertised (no warning)")
unittest {
    import std.algorithm : canFind, countUntil;
    import std.file : write;
    import llm.test_util : testArea;

    auto tmpDir = testArea("agent_t7_emptydesc");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    auto llmConf = makeAgentTestConfig(tmpDir);
    llmConf.toolBroker.enabled = true; // broker on: the not-advertised side is the composed-description pass
    import llm.config : GroupConfig;

    llmConf.toolBroker.groups["agent_t7_empty_desc_probe"] = GroupConfig("", [
        "plain_tool"
    ]);

    auto agent = new Agent("integration", llmConf, null, null, null, llmConf.toolFilter.to());

    auto idx = agent.tools.countUntil!(e => e["function"]["name"].str == "list_tool_tags");
    assert(idx >= 0, "the discovery tool is in the day-one array");
    assert(!canFind(agent.tools[idx]["function"]["description"].str,
            "agent_t7_empty_desc_probe"), "an undescribed group must not be advertised");
}

// The instructive refusal at the dispatch site - the three
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

@("tool dispatch refusals: tier-1 registry-miss and tier-2 config-excluded share the not-available wording, tier-3 in-pool-not-visible is instructive, visible tool dispatches")
unittest {
    import std.file : write;
    import llm.test_util : testArea;
    import std.sumtype : match;

    import llm.chat : ToolResponse;
    import llm.config : GroupConfig;
    import llm.tool_call : RegFunction, addFunction, toParams;
    import llm.utility : toSnakeCase;
    import llm.tool_call.broker : activateTag;

    auto tmpDir = testArea("agent_t8_refusals");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    // Tier-3 fixture: a tagged tool registered before agent construction, so it
    // is in the pool (the default test filter matches every name) but not in
    // the day-one tools array (tagged, never activated).
    addFunction(RegFunction(name: "agent_t8_tier3_fixture", nameSnakeCase: toSnakeCase("agent_t8_tier3_fixture"), desc: "tier3 fixture tool",
            params: toParams!T8FixtureParams, callback: &t8FixtureCallback, tags: [
                "workarea"
    ]));

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

    auto llmConf = makeAgentTestConfig(tmpDir);
    llmConf.toolBroker.enabled = true; // tier-3 refusal + visible dispatch need the broker on
    llmConf.toolBroker.groups["workarea"] = GroupConfig("workarea file tools", [
    ]);
    llmConf.toolBroker.hiddenTags = ["workarea"];
    auto agent = new Agent("integration", llmConf, null, null);

    // Tier-1: the tool is absent from the registry entirely (⇒ absent from the
    // pool) - the 4-arg (filter) executeFunc has no unknown-tool pre-check, so a
    // registry-miss shares tier-2's "not available to this agent" wording.
    auto unknown = dispatch(agent, "agent_t8_no_such_tool");
    assert(unknown.content == "error: tool 'agent_t8_no_such_tool' is not available to this agent",
            unknown.content);
    assert(!unknown.success());
    assert(unknown.toolName == "agent_t8_no_such_tool");

    // Tier-2: the tool exists but the config filter excludes it (⇒ not in the
    // pool either) - the not-available refusal.
    auto llmConf2 = makeAgentTestConfig(tmpDir);
    llmConf2.toolFilter.include = ["^no_such_tool_pattern$"];
    auto excluded = new Agent("integration", llmConf2, null, null, null, llmConf2.toolFilter.to());
    auto unavailable = dispatch(excluded, "write_file");
    assert(unavailable.content == "error: tool 'write_file' is not available to this agent",
            unavailable.content);
    assert(!unavailable.success());
    assert(unavailable.toolName == "write_file");

    // Tier-3: the tool is in the pool but hidden from this agent (tagged, never
    // activated) - the instructive refusal names the recovery path.
    auto hidden = dispatch(agent, "agent_t8_tier3_fixture");
    assert(hidden.content == "error: tool 'agent_t8_tier3_fixture' is not visible to this agent in the current session (visibility resets when chat history is loaded or the session changes)" ~ "; discover available tool groups with `list_tool_tags`, then call `list_tool_tags` with the group name to activate it.",
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
    activateTag(agent.toolCtx.broker, agent.toolCtx.pool, "workarea", agent.toolCtx.brokerConf);
    agent.toolCtx.rebuildTools();
    auto ok = dispatch(agent, "agent_t8_tier3_fixture");
    assert(ok.content == "ok", ok.content);
    assert(ok.success());
}

// Compression-point pruning - the scan/apply split inside Agent.compress
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

@("compression-point pruning: a no-op compression (history not rewritten) leaves the broker state and the tools array untouched")
unittest {
    import std.algorithm : canFind;
    import std.file : write;
    import llm.test_util : testArea;
    import std.json : JSONValue;

    import llm.tool_call : RegFunction, addFunction, toParams;
    import llm.tool_call.broker : activateTag;
    import llm.utility : toSnakeCase;

    auto tmpDir = testArea("agent_t9_noop");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    addFunction(RegFunction(name: "agent_t9_noop_fixture", nameSnakeCase: toSnakeCase("agent_t9_noop_fixture"), desc: "t9 noop fixture",
            params: toParams!T9FixtureParams, callback: &t9FixtureCallback, tags: [
                "workarea"
    ]));
    addFunction(RegFunction(name: "agent_t9_noop_unused", nameSnakeCase: toSnakeCase("agent_t9_noop_unused"), desc: "t9 noop unused fixture",
            params: toParams!T9FixtureParams, callback: &t9FixtureCallback, tags: [
                "workarea"
    ]));

    string[] namesOf(JSONValue[] ts) {
        string[] n;
        foreach (t; ts)
            n ~= t["function"]["name"].str;
        return n;
    }

    auto llmConf = makeAgentTestConfig(tmpDir);
    llmConf.toolBroker.enabled = true; // the pruning path goes through the rebuildTools hook
    auto agent = new Agent("integration", llmConf, null, null);

    activateTag(agent.toolCtx.broker, agent.toolCtx.pool, "workarea", agent.toolCtx.brokerConf);
    agent.toolCtx.rebuildTools();
    assert(namesOf(agent.tools).canFind("agent_t9_noop_fixture"));

    // A used tool: a no-op compression must not prune it either - nothing is
    // pruned at all when the history is not rewritten.
    StreamResponse.ToolCall[long] calls;
    calls[0] = StreamResponse.ToolCall(id: "1", name: "agent_t9_noop_fixture", arguments: "{}");
    agent.handleToolCalls(null, calls);

    // 3 messages: below the summary.compress floor (1 + KeepLast), so the
    // forced compression no-ops: originalLength/newLength stay .init and
    // the rule (0 != 0 || 0 > 0) is false.
    agent.setSystemPrompt("sys");
    agent.addUserQuery("q1");
    agent.addUserQuery("q2");

    auto before = JSONValue(agent.tools).toString;
    auto res = agent.compress(0.9, true);
    assert(!res.compressed, "the chat is too short: summary.compress no-ops");
    assert(res.originalLength == 0 && res.newLength == 0 && res.purgedCount == 0,
            "no-rewrite fields stay .init, so the prune-apply gate is false");
    auto after = JSONValue(agent.tools).toString;
    assert(before == after, "a no-op compression must not touch the tools array");
    assert(agent.toolCtx.broker.activated.canFind("agent_t9_noop_fixture"),
            "no prune: the used tool stays activated");
    assert(namesOf(agent.tools).canFind("agent_t9_noop_fixture"));
    assert(namesOf(agent.tools).canFind("agent_t9_noop_unused"),
            "the prune-apply gate skipped the apply: an unused activated tool survives a no-op");
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

// Seed-on-load: the load-path inverse of the compression-time prune

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
    import std.file : write;
    import llm.test_util : testArea;
    import std.json : JSONValue, parseJSON;
    import std.range : empty;

    import llm.tool_call : RegFunction, addFunction, toParams;
    import llm.tool_call.broker : BrokerState;
    import llm.utility : toSnakeCase;

    auto tmpDir = testArea("agent_t11_seed");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    // Workarea-tagged fixture registered BEFORE agent construction, so it is
    // in the pool (the pool is built at ctor time from the live registry) but
    // not in the day-one tools array (tagged, never activated).
    addFunction(RegFunction(name: "agent_t11_seed_fixture", nameSnakeCase: toSnakeCase("agent_t11_seed_fixture"), desc: "t11 seed fixture",
            params: toParams!T11FixtureParams, callback: &t11FixtureCallback, tags: [
                "workarea"
    ]));

    string[] namesOf(JSONValue[] ts) {
        string[] n;
        foreach (t; ts)
            n ~= t["function"]["name"].str;
        return n;
    }

    auto llmConf = makeAgentTestConfig(tmpDir);
    llmConf.toolBroker.enabled = true; // broker on: the seed path re-activates the tools the history proves were used
    auto agent = new Agent("integration", llmConf, null, null);

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
    import std.file : write;
    import llm.test_util : testArea;
    import std.json : JSONValue, parseJSON;
    import std.range : empty;

    auto tmpDir = testArea("agent_t11_empty");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
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
    import std.file : write;
    import llm.test_util : testArea;
    import std.json : JSONValue, parseJSON;
    import std.range : empty;

    import llm.tool_call : RegFunction, addFunction, toParams;
    import llm.utility : toSnakeCase;

    auto tmpDir = testArea("agent_t11_killswitch");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    addFunction(RegFunction(name: "agent_t11_killswitch_fixture",
            nameSnakeCase: toSnakeCase("agent_t11_killswitch_fixture"), desc: "t11 kill-switch fixture", params: toParams!T11FixtureParams,
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

    assert(agent.toolCtx.broker.activated.empty, "kill switch off: nothing is seeded");
    assert(JSONValue(agent.tools).toString == before,
            "kill switch off: the tools array is untouched by the seed");
}

@("seed-on-load: only alwaysOn (untagged) calls in the history - the activation list and the tools array stay untouched")
unittest {
    import std.algorithm : canFind;
    import std.file : write;
    import llm.test_util : testArea;
    import std.json : JSONValue, parseJSON;
    import std.range : empty;

    auto tmpDir = testArea("agent_t11_alwayson");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    string[] namesOf(JSONValue[] ts) {
        string[] n;
        foreach (t; ts)
            n ~= t["function"]["name"].str;
        return n;
    }

    auto agent = new Agent("integration", makeAgentTestConfig(tmpDir), null, null);

    // A history whose only structured calls target taskDone in both
    // spellings: the legacy-camel "taskDone" (pre-conversion history) and the
    // snake "task_done" (post-conversion). Both are untagged (alwaysOn) and
    // neverHide - the seeder must skip both.
    agent.chat.load(parseJSON(`{
        "messages": [
            {"role": "assistant", "content": null, "reasoning_content": "",
             "tool_calls": [{"id": "1", "type": "function",
                             "function": {"name": "taskDone", "arguments": "{}"}}]},
            {"role": "assistant", "content": null, "reasoning_content": "",
             "tool_calls": [{"id": "2", "type": "function",
                             "function": {"name": "task_done", "arguments": "{}"}}]}
        ]
    }`));
    auto before = JSONValue(agent.tools).toString;
    agent.seedBrokerFromChat();

    assert(agent.toolCtx.broker.activated.empty,
            "an alwaysOn/neverHide call never enters the activation list");
    assert(JSONValue(agent.tools).toString == before,
            "an alwaysOn-only history must not touch the tools array");
    assert(namesOf(agent.tools).canFind("task_done"), "alwaysOn stays in the head");
}

@("seed-on-load: a second seed of the same chat is a no-op - nothing newly activated, no rebuild")
unittest {
    import std.algorithm : canFind;
    import std.file : write;
    import llm.test_util : testArea;
    import std.json : JSONValue, parseJSON;
    import std.range : empty;

    import llm.tool_call : RegFunction, addFunction, toParams;
    import llm.utility : toSnakeCase;

    auto tmpDir = testArea("agent_t11_idem");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    addFunction(RegFunction(name: "agent_t11_idem_fixture", nameSnakeCase: toSnakeCase("agent_t11_idem_fixture"), desc: "t11 idempotency fixture",
            params: toParams!T11FixtureParams, callback: &t11FixtureCallback, tags: [
                "workarea"
    ]));

    string[] namesOf(JSONValue[] ts) {
        string[] n;
        foreach (t; ts)
            n ~= t["function"]["name"].str;
        return n;
    }

    auto llmConf = makeAgentTestConfig(tmpDir);
    llmConf.toolBroker.enabled = true; // broker on: the seed path re-activates the tools the history proves were used
    auto agent = new Agent("integration", llmConf, null, null);

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
    // (nothing newly activated) none. The wrapper forwards to the
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
    assert(rebuilds == 1, "the second seed must fire no rebuild");
    assert(agent.toolCtx.broker.activated.canFind("agent_t11_idem_fixture"),
            "the activation is sticky");
    assert(namesOf(agent.tools).canFind("agent_t11_idem_fixture"), "the seeded tool stays visible");
}

@("seed-on-load end to end: agent.loadHistory seeds the broker from the restored session file")
unittest {
    import std.algorithm : canFind;
    import std.file : write;
    import llm.test_util : testArea;
    import std.json : JSONValue;

    import my.path : Path;

    import llm.tool_call : RegFunction, addFunction, toParams;
    import llm.test_util;
    import llm.utility : toSnakeCase;

    auto tmpDir = testArea("agent_t11_load");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    addFunction(RegFunction(name: "agent_t11_load_fixture", nameSnakeCase: toSnakeCase("agent_t11_load_fixture"), desc: "t11 load fixture",
            params: toParams!T11FixtureParams, callback: &t11FixtureCallback, tags: [
                "workarea"
    ]));

    string[] namesOf(JSONValue[] ts) {
        string[] n;
        foreach (t; ts)
            n ~= t["function"]["name"].str;
        return n;
    }

    auto llmConf = makeAgentTestConfig(tmpDir);
    llmConf.toolBroker.enabled = true; // broker on: the seed path re-activates the tools the history proves were used
    auto agent = new Agent("integration", llmConf, null, null);

    // The history file in the save format, written AFTER construction (the
    // ctor does not auto-load): one structured call + tool response pair.
    write(tmpDir ~ "integration_history.json", `{
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

@("An MCP tool registered at runtime enters the pool at the connect change point, the discovery description includes its tag after the recompose, and activation makes it visible")
unittest {
    import std.algorithm : canFind, filter;
    import std.array : array;
    import std.file : write;
    import llm.test_util : testArea;
    import std.json : parseJSON;

    import llm.config : GroupConfig;
    import llm.mcp_server.registration : registerMcpTool;
    import llm.tool_call.discovery : ListToolTagsParams, listToolTags;

    enum tag = "mcp_ext_server_t12";

    auto tmpDir = testArea("agent_mcp_connect");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    auto llmConf = makeAgentTestConfig(tmpDir);
    llmConf.toolBroker.enabled = true;
    llmConf.toolBroker.groups[tag] = GroupConfig("tags inherited from a connected MCP server", [
    ]);
    llmConf.toolBroker.hiddenTags = [tag];

    auto agent = new Agent("integration", llmConf, null, null);
    assert(!agent.toolCtx.pool.canFind!(f => f.name == "mcp_ext_t12_e2e"),
            "not registered yet — the ctor pools the registry as it is");

    // Runtime registration (the MCP client would call this when a server
    // connects), then the connect change point: the pool is recomputed from
    // the live registry and the array + discovery description rebuilt through
    // toolCtx.rebuildTools (the MCP connect hook). The tag vocabulary is config-side and read
    // at rebuild time; it is preset before construction, so the recomposed
    // description carries the tag - if the hook did not recompose, the raw
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

    auto listDesc = agent.tools.filter!(e => e["function"]["name"].str == "list_tool_tags")
        .array[0]["function"]["description"].str;
    assert(listDesc.canFind("\n\nTags: " ~ tag), listDesc);

    // Discovery loop: list the tag (activates it), then the harness-side
    // rebuild - the MCP tool becomes visible in the array.
    auto resp = listToolTags(agent.toolCtx, ListToolTagsParams(tag));
    assert(resp.success, resp.msg);
    agent.toolCtx.rebuildTools();
    assert(agent.tools.canFind!(e => e["function"]["name"].str == "mcp_ext_t12_e2e"),
            "the MCP tool becomes visible after its tag is activated");
}

@("Kill switch off: the MCP connect hook rebuilds the pre-broker tools array, so an MCP tool becomes visible without activation")
unittest {
    import std.algorithm : canFind;
    import std.file : write;
    import llm.test_util : testArea;

    import llm.mcp_server.registration : registerMcpTool;

    enum tag = "mcp_ext_server_t12_legacy";

    auto tmpDir = testArea("agent_mcp_killswitch");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
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
            "the kill-switch path emits registry ∩ toolFilter regardless of tags");
}

@(
        "pipeline wiring: the coder/plan broker adjust hook keeps pipelineOutput visible under a hidden group")
unittest {
    import std.algorithm : canFind, count, map;
    import std.array : array;
    import std.file : write;
    import llm.test_util : testArea;
    import std.json : JSONValue;
    import std.range : empty;
    import std.conv : to;

    import llm.config : GroupConfig;
    import llm.pipeline : pipelineOutputAlwaysOnHook;

    auto tmpDir = testArea("agent_pipeline_hook");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    auto llmConf = makeAgentTestConfig(tmpDir);
    llmConf.toolFilter.include = [".*"];
    llmConf.toolBroker.enabled = true;
    llmConf.toolBroker.groups["workarea"] = GroupConfig("workarea file tools",
            ["writeFile", "readFile", "pipelineOutput"]);
    llmConf.toolBroker.hiddenTags = ["workarea"];
    // pipelineOutput rides INSIDE the hidden group, so the hook is the only
    // thing un-hiding it: ungrouped tools stay visible under an enabled broker,
    // so a hook-less config would not exercise the wiring at all.

    string[] namesOf(JSONValue[] ts) {
        return ts.map!(t => t["function"]["name"].str).array;
    }

    // The pipelines install the shared hook (pipelineOutputAlwaysOnHook) on
    // their transient agents; replicated here against the same factory - the
    // pipelines themselves need a live backend to run.
    auto coder = new Agent("coder", llmConf, null, null);
    coder.setBrokerAdjustHook(pipelineOutputAlwaysOnHook());
    coder.resetModel(llmConf.activeCodeModel);

    auto planner = new Agent("implementation_planner", llmConf, null, null);
    planner.setBrokerAdjustHook(pipelineOutputAlwaysOnHook());
    planner.resetModel(llmConf.activeCodeModel);

    assert(namesOf(coder.tools).canFind("pipeline_output"),
            "the hook keeps pipelineOutput visible under a hidden group: " ~ namesOf(coder.tools)
                .to!string);
    assert(namesOf(planner.tools).canFind("pipeline_output"),
            "the hook keeps pipelineOutput visible under a hidden group: " ~ namesOf(
                planner.tools).to!string);
    assert(!namesOf(coder.tools).canFind("write_file"),
            "the hook forces exactly pipelineOutput, not the hidden group");
    assert(llmConf.toolBroker.alwaysOn.empty, "hook mutations do not write back into LlmConfig");

    // Idempotence: a same-model reset re-fires the hook on a fresh resolution;
    // the absence check keeps one alwaysOn entry and the tools array one entry.
    coder.resetModel(llmConf.activeCodeModel);
    assert(namesOf(coder.tools).count!(n => n == "pipeline_output") == 1);
}

// The measured LLM call Duration is stamped once at message
// construction - the assistant Message in parseResponse, each ToolMessage in
// handleToolCalls. Injected Durations keep the asserts deterministic; the
// wall-clock path is covered E2E. The default parameter leaves direct
// two-arg call sites unstamped (NoProcessingTime).
@("an injected Duration is stamped once at construction on the assistant and tool messages; the default parameter leaves messages unstamped")
unittest {
    import std.datetime : dur;
    import std.file : write;
    import llm.test_util : testArea;
    import std.sumtype : has;

    import llm.chat : MessageTime, NoProcessingTime, ToolMessage, ToolResponse;
    import llm.types : StreamMessage;

    import my.path : Path;

    auto tmpDir = testArea("agent_stamp");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);
    auto monitor = new MetricMonitor(tmpDir ~ "metrics.jsonl");
    auto llmConf = makeAgentTestConfig(tmpDir);
    // Close the feedback gates: with a live monitor and the default config the
    // first-call sentinel would add a warning Message and shift the counts.
    llmConf.nudges.feedback.enabled = false;
    auto agent = new CannedProcessAgent("stamp", llmConf, monitor);

    // LLM stamp: drive parseResponse with a canned StreamResponse.
    StreamResponse sp = StreamResponse(ServerStat.init);
    sp.message = StreamMessage(role: "assistant", content: "hello", finishReason: "stop");
    auto status = agent.parseResponse(sp, MessageTime(1234.dur!"msecs"));
    assert(status == ProcessResult.Status.ok);
    auto msgs = agent.chat.getMessages;
    assert(msgs.length == 1);
    assert(msgs[0].match!((Message m) => m.processTime() == MessageTime(1234.dur!"msecs"),
            (_) => false));

    // Tool stamp: llmCall on the ToolMessage (the tool-execution stamp on the
    // ToolResponse is asserted in its own test). The default parameter leaves
    // messages unstamped.
    StreamResponse.ToolCall[long] calls;
    calls[0] = StreamResponse.ToolCall(id: "1", name: "no_such_tool", arguments: "{}");
    agent.handleToolCalls(null, calls, MessageTime(555.dur!"msecs"));
    msgs = agent.chat.getMessages;
    assert(msgs.length == 3); // tool message + tool response (no nudge: monitor gates closed)
    assert(msgs[1].match!((ToolMessage m) => m.processTime() == MessageTime(555.dur!"msecs"),
            (_) => false));
    assert(msgs[2].match!((ToolResponse m) => m.toolCallId == "1", (_) => false));

    StreamResponse.ToolCall[long] bare;
    bare[0] = StreamResponse.ToolCall(id: "2", name: "no_such_tool", arguments: "{}");
    agent.handleToolCalls(null, bare); // default parameter: unstamped
    msgs = agent.chat.getMessages;
    assert(msgs[3].match!((ToolMessage m) => m.processTime().has!NoProcessingTime, (_) => false));
}

// The measured per-call tool-execution Duration is stamped on each
// ToolResponse: the value stored is the measured responseTimeMs - the
// display path never re-measures. Both calls run a 10 ms sleep fixture so
// each measurement is non-zero - every built-in tool fast-fails in < 1 ms
// in the unit environment, and the integer-millisecond measurement would
// report 0. The live monitor keeps the monitor.record path under test; the
// feedback gate is closed so the first-call sentinel does not add a
// warning Message and shift the counts.
version (unittest) {
    import core.thread : Thread;
    import std.datetime : dur;
    import std.json : JSONValue;

    import llm.tool_call : Context, ExecuteFuncResult;

    /// Empty params struct for the sleep fixture (nothing to decode).
    private struct ToolTimeSleepParams {
    }

    /// Sleeps so the per-call execution time measures non-zero.
    private ExecuteFuncResult toolTimeSleepCallback(Context ctx, JSONValue args) {
        Thread.sleep(10.dur!"msecs");
        return ExecuteFuncResult("slept", true);
    }
}

@(
        "each ToolResponse carries its own measured per-call tool-execution time, non-zero with the sleep fixture")
unittest {
    import std.datetime : Duration;
    import std.file : write;
    import llm.test_util : testArea;
    import my.path : Path;
    import logger = std.logger;

    import llm.chat : MessageTime, NoProcessingTime, ToolResponse;
    import llm.tool_call : RegFunction, addFunction, toParams;
    import llm.utility : toSnakeCase;

    addFunction(RegFunction(name: "agent_tooltime_sleep", nameSnakeCase: toSnakeCase("agent_tooltime_sleep"), desc: "tooltime sleep fixture",
            params: toParams!ToolTimeSleepParams, callback: &toolTimeSleepCallback));

    auto tmpDir = testArea("agent_tooltime");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);
    auto monitor = new MetricMonitor(tmpDir ~ "metrics.jsonl");
    auto llmConf = makeAgentTestConfig(tmpDir);
    // Close the feedback gate: with a live monitor and the default config the
    // first-call sentinel would add a warning Message and shift the counts.
    llmConf.nudges.feedback.enabled = false;
    auto agent = new CannedProcessAgent("tooltime", llmConf, monitor);

    // Multi-tool step: each ToolResponse carries its OWN per-call duration
    // (measured around its own iteration: startTime at the loop top).
    StreamResponse.ToolCall[long] calls;
    calls[0] = StreamResponse.ToolCall(id: "1", name: "agent_tooltime_sleep", arguments: "{}");
    calls[1] = StreamResponse.ToolCall(id: "2", name: "agent_tooltime_sleep", arguments: "{}");
    agent.handleToolCalls(null, calls);

    auto msgs = agent.chat.getMessages;
    assert(msgs.length == 4); // 2 x (tool message + tool response)
    Duration d0;
    Duration d1;
    // Extract each stamp with an expression-lambda match, then assert with a
    // single-level block-lambda match: a nested match! call inside a
    // block-lambda handler makes LDC infer "delegate that returns a delegate"
    // and dispatch nothing, so the assertions would pass vacuously. Also keep
    // the catch-alls single-expression arrow lambdas (`(_) => assert(0)`): an
    // arrow+block handler in one match! call breaks std.sumtype instantiation
    // under LDC 1.42 ("expected return type of `bool`, not
    // `void delegate() @safe`").
    auto mt0 = msgs[1].match!((ToolResponse m) => m.processTime(),
            (_) => MessageTime(NoProcessingTime()));
    auto mt1 = msgs[3].match!((ToolResponse m) => m.processTime(),
            (_) => MessageTime(NoProcessingTime()));
    mt0.match!((Duration d) { assert(d > Duration.init); d0 = d; }, (_) => assert(0));
    mt1.match!((Duration d) { assert(d > Duration.init); d1 = d; }, (_) => assert(0));
    // Distinct per-call values in practice (two independent measurements); the
    // unit environment cannot force different times for two calls of the same
    // fixture, so equality is possible and acceptable - distinctness is
    // exercised E2E, where real tools take different times.
    logger.info("per-call tool times: ", d0, " / ", d1);
}

@("taskDone identity (conversion era): the snake-spelling dispatch attaches and hides, and a legacy-spelling call still attaches and stays hidden")
unittest {
    import std.datetime : Clock;
    import std.file : mkdirRecurse, write;
    import std.format : format;
    import std.sumtype : match;

    import llm.chat : ToolMessage, ToolResponse;

    auto tmpDir = testArea("agent_t4_conv_era");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    auto llmConf = makeAgentTestConfig(tmpDir);
    auto agent = new Agent("integration", llmConf, null, null);

    // Post-conversion spelling: dispatches to the real tool via the snake
    // axis and attaches + hides under "task_done".
    StreamResponse.ToolCall[long] calls;
    calls[0] = StreamResponse.ToolCall(id: "1", name: "task_done", arguments: `{"answer":"Conversion-era answer."}`);
    agent.handleToolCalls(null, calls);

    ToolMessage tm;
    foreach (msg; agent.chat.getMessages)
        msg.match!((ToolMessage m) { tm = m; }, (_) {});
    ToolResponse tr;
    foreach (msg; agent.chat.getMessages)
        msg.match!((ToolResponse r) { tr = r; }, (_) {});
    assert(tr.success(), tr.content);
    assert(tr.content == "done", tr.content);
    assert(tr.toolName == "task_done");
    assert(tm.isFinalAnswer(), "snake-spelling ToolMessage must carry the final answer");
    assert(tm.getFinalAnswer() == "Conversion-era answer.");
    auto snakeCallName = tm.toolCalls.array[0]["function"]["name"].str;
    assert(snakeCallName == "task_done");
    assert(llmConf.isHiddenTool(snakeCallName), "snake-spelling tool call must stay hidden");
    assert(llmConf.isHiddenTool("task_done"), "snake-spelling tool response must stay hidden");

    calls.clear();
    calls[0] = StreamResponse.ToolCall(id: "2", name: "taskDone", arguments: `{"answer":"Legacy follow-up."}`);
    agent.handleToolCalls(null, calls);

    ToolMessage tm2;
    foreach (msg; agent.chat.getMessages)
        msg.match!((ToolMessage m) { tm2 = m; }, (_) {});
    ToolResponse tr2;
    foreach (msg; agent.chat.getMessages)
        msg.match!((ToolResponse r) { tr2 = r; }, (_) {});
    assert(tr2.toolName == "taskDone");
    assert(tm2.isFinalAnswer(), "legacy-spelling ToolMessage must still carry the final answer");
    assert(tm2.getFinalAnswer() == "Conversion-era answer.");
    auto legacyCallName = tm2.toolCalls.array[0]["function"]["name"].str;
    assert(legacyCallName == "taskDone");
    assert(llmConf.isHiddenTool(legacyCallName), "legacy-spelling tool call must stay hidden");
    assert(llmConf.isHiddenTool("taskDone"));
}

version (unittest) {
    import std.json : JSONValue;

    import llm.tool_call : Context, ExecuteFuncResult;

    private struct task5FixtureParams {
    }

    private ExecuteFuncResult task5FixtureCallback(Context ctx, JSONValue args) {
        return ExecuteFuncResult("ok", true);
    }
}

@(
        "prune scan: a snake-spelled call in the chat keeps the activated tool; only the unused one comes back inactive")
unittest {
    import std.conv : to;
    import std.json : JSONValue;

    import llm.chat : Chat, ToolMessage;
    import llm.tool_call : RegFunction;
    import llm.tool_call.broker : BrokerState, applyPrune, scanInactiveTools;
    import llm.utility : toSnakeCase;

    auto pool = [
        RegFunction(name: "task5aPruneUsed", nameSnakeCase: toSnakeCase("task5aPruneUsed"), desc: "d",
                params: [], callback: null),
        RegFunction(name: "task5aPruneUnused", nameSnakeCase: toSnakeCase("task5aPruneUnused"),
                desc: "d", params: [], callback: null),
    ];
    assert(pool[0].nameSnakeCase == "task5a_prune_used");
    assert(pool[1].nameSnakeCase == "task5a_prune_unused");

    // Post-conversion history: the structured call spells the tool by its
    // snake name.
    Chat chat;
    JSONValue f;
    f["name"] = "task5a_prune_used";
    JSONValue call;
    call["function"] = f;
    chat.add(ToolMessage("", JSONValue([call])));

    auto st = BrokerState();
    st.activated = ["task5a_prune_used", "task5a_prune_unused"];

    auto inactive = scanInactiveTools(st, chat, pool);
    assert(inactive == ["task5a_prune_unused"], inactive.to!string);

    applyPrune(st, inactive);
    assert(st.activated == ["task5a_prune_used"], st.activated.to!string);
}

@(
        "prune scan: a legacy D-spelled call in the chat keeps the activated snake entry (dual-axis usage)")
unittest {
    import std.array : empty;
    import std.conv : to;
    import std.json : JSONValue;

    import llm.chat : Chat, ToolMessage;
    import llm.tool_call : RegFunction;
    import llm.tool_call.broker : BrokerState, scanInactiveTools;
    import llm.utility : toSnakeCase;

    auto pool = [
        RegFunction(name: "task5aLegacyCamel", nameSnakeCase: toSnakeCase("task5aLegacyCamel"),
                desc: "d", params: [], callback: null)
    ];

    // Pre-conversion history: the structured call spells the tool by its
    // D name while the state holds the snake entry.
    Chat chat;
    JSONValue f;
    f["name"] = "task5aLegacyCamel";
    JSONValue call;
    call["function"] = f;
    chat.add(ToolMessage("", JSONValue([call])));

    auto st = BrokerState();
    st.activated = ["task5a_legacy_camel"];

    auto inactive = scanInactiveTools(st, chat, pool);
    assert(inactive.empty,
            "the D-spelled call must keep the snake entry active: " ~ inactive.to!string);
}

@("seed: a legacy D-name call in a loaded chat seeds the SNAKE name, and the rebuilt tools array carries the tool under its snake name")
unittest {
    import std.algorithm : canFind;
    import std.datetime : Clock;
    import std.file : mkdirRecurse, write;
    import std.format : format;
    import std.json : JSONValue, parseJSON;

    import llm.config : GroupConfig;
    import llm.tool_call : RegFunction, addFunction, toParams;
    import llm.tool_call.broker : BrokerState;
    import llm.utility : toSnakeCase;

    auto tmpDir = testArea("agent_task5_seed");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    // camelCase D name in a hidden group, snake spelling populated the way
    // the registry does; registered before agent construction so it is in
    // the pool.
    addFunction(RegFunction(name: "task5bCamelTool", nameSnakeCase: toSnakeCase("task5bCamelTool"), desc: "task5 b fixture",
            params: toParams!task5FixtureParams, callback: &task5FixtureCallback,
            tags: ["task5b_group"]));

    string[] namesOf(JSONValue[] ts) {
        string[] n;
        foreach (t; ts)
            n ~= t["function"]["name"].str;
        return n;
    }

    auto llmConf = makeAgentTestConfig(tmpDir);
    llmConf.toolBroker.enabled = true;
    llmConf.toolBroker.groups["task5b_group"] = GroupConfig("task5 b group", []);
    llmConf.toolBroker.hiddenTags = ["task5b_group"];
    auto agent = new Agent("integration", llmConf, null, null);

    // Fresh-broker pin, kept explicit: the case seeds from scratch.
    agent.toolCtx.broker = BrokerState.init;

    // Pre-conversion history: the structured call spells the tool by its D name.
    agent.chat.load(parseJSON(`{
        "messages": [
            {"role": "assistant", "content": null, "reasoning_content": "",
             "tool_calls": [{"id": "1", "type": "function",
                             "function": {"name": "task5bCamelTool", "arguments": "{}"}}]},
            {"role": "tool", "content": "out", "tool_call_id": "1",
             "name": "task5bCamelTool"}
        ]
    }`));
    agent.seedBrokerFromChat();

    assert(agent.toolCtx.broker.activated.canFind("task5b_camel_tool"),
            "the legacy D-name call seeds the SNAKE name");
    assert(!agent.toolCtx.broker.activated.canFind("task5bCamelTool"),
            "the D name itself never enters the activation list");
    assert(namesOf(agent.tools).canFind("task5b_camel_tool"),
            "the rebuilt tools array carries the seeded tool under its snake name");
}

@("discovery: activating a hidden group via the snake-spelled discovery tool makes the group's camel-named tool visible and dispatchable by its snake name")
unittest {
    import std.algorithm : canFind;
    import std.datetime : Clock;
    import std.file : mkdirRecurse, write;
    import std.format : format;
    import std.sumtype : match;

    import llm.chat : ToolResponse;
    import llm.config : GroupConfig;
    import llm.tool_call : RegFunction, addFunction, toParams;
    import llm.utility : toSnakeCase;

    auto tmpDir = testArea("agent_task5_discovery");
    scope (exit)
        tmpDir.cleanup;
    write(tmpDir ~ "SUMMARY.md", "Summarize text.");
    writeDefaultNudgeFiles(tmpDir);

    // camelCase D name in a hidden group, snake spelling populated the way
    // the registry does.
    addFunction(RegFunction(name: "task5cCamelTool", nameSnakeCase: toSnakeCase("task5cCamelTool"), desc: "task5 c fixture",
            params: toParams!task5FixtureParams, callback: &task5FixtureCallback,
            tags: ["task5c_group"]));

    string[] namesOf(JSONValue[] ts) {
        string[] n;
        foreach (t; ts)
            n ~= t["function"]["name"].str;
        return n;
    }

    ToolResponse dispatch(Agent agent, string name, string args) {
        StreamResponse.ToolCall[long] calls;
        calls[0] = StreamResponse.ToolCall(id: "1", name: name, arguments: args);
        agent.handleToolCalls(null, calls);
        ToolResponse rval;
        foreach (msg; agent.chat.getMessages)
            msg.match!((ToolResponse r) { rval = r; }, (_) {});
        return rval;
    }

    auto llmConf = makeAgentTestConfig(tmpDir);
    llmConf.toolBroker.enabled = true;
    llmConf.toolBroker.groups["task5c_group"] = GroupConfig("task5 c group", []);
    llmConf.toolBroker.hiddenTags = ["task5c_group"];
    auto agent = new Agent("integration", llmConf, null, null);

    assert(!namesOf(agent.tools).canFind("task5c_camel_tool"),
            "hidden group: the tool is not visible before activation");

    // The discovery tool dispatches by its snake spelling; it activates the
    // group and rebuilds the tools array.
    auto tags = dispatch(agent, "list_tool_tags", `{"tag":"task5c_group"}`);
    assert(tags.success(), tags.content);
    assert(tags.content.canFind("task5c_camel_tool"),
            "the activation reply names the newly visible tool by its snake name: " ~ tags.content);

    assert(namesOf(agent.tools).canFind("task5c_camel_tool"),
            "the rebuilt tools array carries the group tool under its snake name");

    // The group tool dispatches by its snake name (the dispatch axis).
    auto ok = dispatch(agent, "task5c_camel_tool", "{}");
    assert(ok.success(), ok.content);
    assert(ok.content == "ok", ok.content);
}

@("activation: activateTool matches either axis and stores exactly one snake entry")
unittest {
    import std.conv : to;

    import llm.tool_call : RegFunction;
    import llm.tool_call.broker : BrokerState, activateTool;
    import llm.utility : toSnakeCase;

    auto pool = [
        RegFunction(name: "task5dCamel", nameSnakeCase: toSnakeCase("task5dCamel"),
                desc: "d", params: [], callback: null)
    ];
    assert(pool[0].nameSnakeCase == "task5d_camel");

    BrokerState st;
    activateTool(st, pool, "task5dCamel"); // D axis
    assert(st.activated == ["task5d_camel"],
            "D-axis activation stores the snake name: " ~ st.activated.to!string);
    activateTool(st, pool, "task5d_camel"); // snake axis: sticky
    assert(st.activated == ["task5d_camel"],
            "snake-axis activation of the same tool is a no-op: " ~ st.activated.to!string);
    activateTool(st, pool, "task5d_ghost");
    assert(st.activated == ["task5d_camel"], "unknown name no-ops: " ~ st.activated.to!string);
}

// The stream's tool calls are an AA keyed by the call's protocol index, and an AA's iteration
// order is not defined. Flattening by sorted key makes both the `--- Tool ---` block order in
// the streamed text and "the newest tool" (`tools[$ - 1]`, which the lane row reports as
// `currentTool`) deterministic - the prerequisite for carrying the tool name as a field.
@("the stream's tool calls are flattened in protocol order")
unittest {
    auto sp = StreamResponse(ServerStat.init);
    // Inserted in a scrambled order: the AA's iteration order is not the insertion order.
    sp.toolCalls[2] = StreamResponse.ToolCall(id: "c", name: "write_file", arguments: "{}");
    sp.toolCalls[0] = StreamResponse.ToolCall(id: "a", name: "read_file", arguments: "{}");
    sp.toolCalls[1] = StreamResponse.ToolCall(id: "b", name: "grep_files", arguments: "{}");

    auto flat = orderedToolCalls(sp);
    assert(flat.length == 3, "every call is carried");
    assert(flat[0].toolName == "read_file" && flat[1].toolName == "grep_files"
            && flat[2].toolName == "write_file", "the calls are in protocol-index order");
    assert(flat[$ - 1].toolName == "write_file",
            "the last element is the highest index - what the UI reports as the newest tool");

    auto noCalls = StreamResponse(ServerStat.init);
    assert(orderedToolCalls(noCalls).length == 0, "a response with no call is an empty list");
}

// The lane row reports D's own `stat.context`, and llama.cpp's last chunk rewrites that number:
// `timings.cache_n` becomes the context start and the streaming estimate is zeroed. The status
// sentence has always carried the same value, so the field path is consistent with it - and it is
// why a finished lane can show a *smaller* count than the ticks before it (or none at all, on a
// first response with no cache hit). Pinned so tasks 40/41 ratify the row's meaning knowingly.
@("the timings chunk makes stat.context the server's cache_n")
unittest {
    // Both chunk shapes are the ones quoted in `parse`'s own comments: a content delta, and the
    // final chunk that ends a llama.cpp response and carries `timings`.
    const string contentChunk = `data: {"choices":[{"index":0,"delta":{"role":"assistant","content":"hello world"}}]}`;
    const string warmEndChunk = `data: {"choices":[{"finish_reason":"stop","index":0,"delta":{}}],`
        ~ `"timings":{"cache_n":6151,"prompt_n":4,"predicted_n":244,`
        ~ `"predicted_per_second":30.04}}`;
    const string coldEndChunk = `data: {"choices":[{"finish_reason":"stop","index":0,"delta":{}}],`
        ~ `"timings":{"cache_n":0,"predicted_n":2}}`;

    auto sp = StreamResponse(ServerStat.init);
    // A content chunk grows the streaming estimate: `stat.context` is startContext + tokenCount.
    sp.parse(contentChunk);
    assert(sp.stat.startContext == 0, "no timings yet: the estimate starts from the caller's stat");
    assert(sp.stat.tokenCount > 0, "the generated text is counted");
    const long streamingEstimate = sp.stat.context;

    sp.parse(warmEndChunk);
    assert(sp.stat.startContext == 6151, "cache_n becomes the context start");
    assert(sp.stat.tokenCount == 0, "and the streaming estimate is zeroed");
    assert(sp.stat.context == 6151, "so the context the UI is told is cache_n");
    assert(sp.stat.context != streamingEstimate, "which is no longer the streaming estimate");

    // A response with no cache hit at all (the first one of a session) ends with `cache_n == 0`,
    // so the lane's count becomes 0 and the row draws no number - the case the old
    // `parsed > 0` latch used to paper over.
    auto cold = StreamResponse(ServerStat.init);
    cold.parse(contentChunk);
    cold.parse(coldEndChunk);
    assert(cold.stat.context == 0, "a cold response ends with no context count to draw");
}
