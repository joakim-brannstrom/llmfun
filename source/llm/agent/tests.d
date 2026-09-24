/// Integration and prompt-data guards for the Agent turn policy.
module llm.agent.tests;

import std.algorithm : canFind;
import std.file : exists;

import llm.agent;
import llm.chat : turnIdOf;

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

// The trigger rule lives in prompt data, not code, so in-tree removal of the section would silently change the main agent's behavior. This guard loads the real llmfun/config/prompt/AGENT.md through the production getPrompt/getBasePrompt path (FlatVfs) and asserts the section heading and the exact tool name are present. A missing file makes getBasePrompt throw, which fails the test.
unittest {
    const agentPromptFile = "llmfun/config/prompt/AGENT.md";
    assert(agentPromptFile.exists,
            "in-tree AGENT.md missing; getBasePrompt would throw at startup (R12)");
    auto llmConf = makeAgentTestConfig("llmfun/config/prompt");
    auto prompt = llmConf.getPrompt(null, "AGENT.md");
    assert(prompt.canFind("# Dialogue History Retrieval"),
            "Task 8 trigger-rule section missing from the composed main-agent prompt (R12)");
    assert(prompt.canFind("queryDialogueHistory"),
            "Task 8 rule must name the queryDialogueHistory tool (R12)");
}

// The reasoning-history rule also lives in prompt data, not code, so in-tree removal of the section would silently change the main agent's behavior. Same guard shape as the dialogue test above: load the real llmfun/config/prompt/AGENT.md through the production getPrompt path and assert the section heading, the exact tool name, and the anti-anchoring warning are present.
unittest {
    const agentPromptFile = "llmfun/config/prompt/AGENT.md";
    assert(agentPromptFile.exists,
            "in-tree AGENT.md missing; getBasePrompt would throw at startup (R12)");
    auto llmConf = makeAgentTestConfig("llmfun/config/prompt");
    auto prompt = llmConf.getPrompt(null, "AGENT.md");
    assert(prompt.canFind("# Reasoning History Retrieval"),
            "Reasoning History Retrieval section missing from the composed main-agent prompt");
    assert(prompt.canFind("queryReasoningHistory"),
            "Reasoning rule must name the queryReasoningHistory tool");
    assert(prompt.canFind("PAST THOUGHTS, NOT ground truth"),
            "Reasoning rule must state that results are past thoughts, not ground truth");
}
