/// Pipeline slash commands: /plan and /code.
/// Self-contained pipeline dispatch (System Designer → Implementation
/// Planner for /plan; Coder → Code Reviewer loop for /code. Both are `SlashArgMode.required` — bare `/plan`/`/code` take the
/// unknown-command path; the handler receives the raw unstripped arg.
module llm.app_agent.slash_pipeline;

import std.conv : text;
import std.functional : toDelegate;

import llm.app_agent; // AgentApp (cyclic package import, same pattern as slash.d)
import llm.app_agent.slash;
import llm.coder : runCoderPipeline;
import llm.pipeline : prettyPrint;
import llm.plan : runPlanPipeline;
import llm.utility : isStopAgentTriggered;

package void registerPipelineCommands(ref SlashCommandRegistry registry) {
    auto ignore = registry.register(SlashCommand("plan", [], [
        "   /plan <query>      Run the plan pipeline"
    ], SlashArgMode.required, 120, toDelegate(&planHandler)));
    ignore = registry.register(SlashCommand("code", [], [
        "   /code <query>      Run the coder pipeline"
    ], SlashArgMode.required, 130, toDelegate(&codeHandler)));
}

/// `/plan <arg>`: run the plan pipeline (System Designer → Implementation
/// Planner). `arg` is the raw remainder after "/plan " (registry
/// tokenization, no strip — the required rule supplies the empty-arg guard).
private AgentStatus planHandler(ref AgentApp app, string arg) {
    app.uiMsg.pipelineClear;
    auto q = arg;
    bool interrupted;
    // The display-only turn opens BEFORE the run's notices so they
    // attach to it (a notice targets the store's newest turn).
    if (app.uiMsg.isActive())
        app.openPipelineTurnTyped(q);
    // The close bracket registers immediately after the open (a throw in
    // between must not strand the turn Running on the wire with /c's
    // displaced context lost), and the failure scope runs FIRST (reverse
    // registration order): the flag is set before the close reads it.
    scope (exit) {
        // Close on every exit (the run-exit bracket discipline), BEFORE the
        // ready edge crosses - runAgent closes the turn (emitRunEndedTyped)
        // before its unwinding releases the composer, so the pipeline
        // brackets keep the same order. The result notice is already in
        // the turn when the scope unwinds.
        if (app.uiMsg.isActive())
            app.closePipelineTurnTyped(interrupted);
        app.agentReady();
    }
    scope (failure)
        interrupted = true; // an exceptional exit is an interrupted run
    app.sendChatMessage("assistant: Running plan pipeline: %s", q);
    app.agentBusy();
    auto result = runPlanPipeline(q, app.llmConf, app.rag, app.monitor, () {
        return isStopAgentTriggered;
    }, app.llmConf.toolFilter.to(), app.makePipelineStreamCallback);
    interrupted = result.wasInterrupted;
    app.sendChatMessage(prettyPrint(result));
    return AgentStatus.active;
}

/// `/code <arg>`: run the coder pipeline (Coder → Code Reviewer loop).
/// Checks `result.wasInterrupted` and emits an interrupted message — the plan
/// branch never checked it (both branches feed the same flag to the
/// display-only turn's close; only the message stays /code-only).
private AgentStatus codeHandler(ref AgentApp app, string arg) {
    app.uiMsg.pipelineClear;
    auto q = arg;
    bool interrupted;
    // Same display-only turn bracket as /plan (planHandler's comment).
    if (app.uiMsg.isActive())
        app.openPipelineTurnTyped(q);
    scope (exit) {
        if (app.uiMsg.isActive())
            app.closePipelineTurnTyped(interrupted);
        app.agentReady();
    }
    scope (failure)
        interrupted = true; // an exceptional exit is an interrupted run
    app.sendChatMessage("assistant: Running coder pipeline: %s", q);
    app.agentBusy();
    auto result = runCoderPipeline(q, app.llmConf, app.rag, app.monitor, () {
        return isStopAgentTriggered;
    }, app.llmConf.toolFilter.to(), app.makePipelineStreamCallback);
    interrupted = result.wasInterrupted;
    if (result.wasInterrupted) {
        app.sendChatMessage("assistant: Pipeline interrupted by user.");
        return AgentStatus.active;
    }
    app.sendChatMessage(i"assistant: $(prettyPrint(result))".text);
    return AgentStatus.active;
}

@("bare /plan and /code take the unknown-command path, and the help lines render in order")
unittest {
    import llm.app_config : UserConfig;
    import std.algorithm.searching : canFind;
    import std.string : indexOf;

    // AgentApp's constructor installs a blocked UiMessenger; the
    // unknown-command path dereferences uiMsg, so a real instance is needed.
    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    SlashCommandRegistry reg;
    registerPipelineCommands(reg);

    // SlashArgMode.required is asserted directly via the registry getter
    // (primary guard — decoupled from dispatch behavior; the stub-based
    // registry test in slash.d pins the required-rule mechanics). The
    // execute calls below are the behavioral fallback: bare /plan and /code
    // (with or without a trailing space — the registry tokenizes "/plan "
    // as an empty arg) take the unknown-command path. Dispatching WITH
    // an argument is not exercised here: the handlers call
    // runPlanPipeline/runCoderPipeline via makePipelineStreamCallback, which
    // dereferences agent_ (only created in run()).
    assert(reg.argModeOf("plan") == SlashArgMode.required);
    assert(reg.argModeOf("code") == SlashArgMode.required);
    assert(reg.execute(app, "/plan") == AgentStatus.active);
    assert(reg.execute(app, "/plan ") == AgentStatus.active);
    assert(reg.execute(app, "/code") == AgentStatus.active);
    assert(reg.execute(app, "/code ") == AgentStatus.active);

    // Help lines are the verbatim strings
    auto help = reg.helpText();
    assert(help.canFind("   /plan <query>      Run the plan pipeline"));
    assert(help.canFind("   /code <query>      Run the coder pipeline"));

    // Help ordering is (order asc, registration index asc): 120, 130
    assert(help.indexOf("   /plan") < help.indexOf("   /code"));
}
