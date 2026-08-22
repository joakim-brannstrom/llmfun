/// Core agent-control slash commands: /quit /q /exit, /compact, /help, /debug, /stop.
module llm.app_agent.slash_core;

import logger = std.logger;

import std.functional : toDelegate;

import llm.app_agent; // AgentApp (cyclic package import, same pattern as slash.d)
import llm.app_agent.slash;
import llm.utility : clearStopAgent, stopAgent;
import llmfun_tui; // TuiChatMessageType_* (C binding)

package void registerCoreCommands(ref SlashCommandRegistry registry) {
    auto ignore = registry.register(SlashCommand("help", [],
            ["   /help              Show this help message"],
            SlashArgMode.none, 10, toDelegate(&helpHandler)));
    ignore = registry.register(SlashCommand("quit", ["q", "exit"],
            ["   /quit, /q, /exit   Exit the agent"], SlashArgMode.none, 20,
            (ref AgentApp app, string arg) => AgentStatus.terminate));
    ignore = registry.register(SlashCommand("stop", [],
            ["   /stop              Stop processing the currently active query"],
            SlashArgMode.none, 30, toDelegate(&stopHandler)));
    ignore = registry.register(SlashCommand("continue", ["c", "cont"],
            ["   /continue, /c      Continue processing the last query"],
            SlashArgMode.none, 30, toDelegate(&continueHandler)));
    ignore = registry.register(SlashCommand("compact", [], [
        "   /compact           Force compress the chat history"
    ], SlashArgMode.none, 40, toDelegate(&compactHandler)));
    ignore = registry.register(SlashCommand("debug", [], [
        "   /debug             Toggle verbose debug output"
    ], SlashArgMode.none, 140, toDelegate(&debugHandler)));
}

/// `/compact`: force-compress the chat history.
private AgentStatus compactHandler(ref AgentApp app, string arg) {
    app.doCompress(true);
    return AgentStatus.active;
}

/// `/help`: print the registry-generated help. The splash gate is preserved
/// (`LLMFUN_NO_SPLASH` env or a CLI prompt suppresses the output).
private AgentStatus helpHandler(ref AgentApp app, string arg) {
    auto helpText = app.printHelp(app.conf_);
    if (helpText !is null) {
        app.sendChatMessage(helpText, TuiChatMessageType_User);
    }
    return AgentStatus.active;
}

/// `/debug`: toggle verbose debug output.
private AgentStatus debugHandler(ref AgentApp app, string arg) {
    app.debugMode = !app.debugMode;
    app.uiMsg.logFile(app.debugMode);
    logger.globalLogLevel = app.debugMode ? logger.LogLevel.trace : logger.LogLevel.info;
    app.sendChatMessage("Debug output: %s", TuiChatMessageType_Assistant,
            app.debugMode ? "ON" : "OFF");
    return AgentStatus.active;
}

/// `/stop`: the TUI intercepts the exact string `/stop` (tui/package.d);
/// this handler covers everything that reaches the agent — `/stop ` with a
/// trailing space and one-shot `-p "/stop"`. Neutral wording: no query is in flight on this path, and calling stopAgent() at idle
/// is harmless (cleared before the next runAgent).
private AgentStatus stopHandler(ref AgentApp app, string arg) {
    stopAgent();
    app.sendChatMessage("harness: Stop requested", TuiChatMessageType_Assistant);
    return AgentStatus.active;
}

/// Instruct the agent to continue working without injecting a user query.
private AgentStatus continueHandler(ref AgentApp app, string arg) {
    app.continueAgent();
    app.sendChatMessage("harness: Continue requested", TuiChatMessageType_Assistant);
    return AgentStatus.active;
}

unittest {
    import llm.app_config : UserConfig;
    import std.algorithm.searching : canFind;
    import std.string : indexOf;

    // AgentApp's constructor installs a blocked UiMessenger (W5); the
    // unknown-command path dereferences uiMsg, so a real instance is needed.
    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    SlashCommandRegistry reg;
    registerCoreCommands(reg);

    // /quit and its aliases terminate
    assert(reg.execute(app, "/quit") == AgentStatus.terminate);
    assert(reg.execute(app, "/q") == AgentStatus.terminate);
    assert(reg.execute(app, "/exit") == AgentStatus.terminate);

    // SlashArgMode.none: exact match only — any arg takes the unknown path
    assert(reg.execute(app, "/quit now") == AgentStatus.active);
    assert(reg.execute(app, "/compact please") == AgentStatus.active);

    // The remaining core handlers dispatch to their handlers. `/compact` is
    // not dispatched here: its handler calls doCompress, which dereferences
    // agent_ (only created in run()).
    assert(reg.execute(app, "/stop") == AgentStatus.active);
    assert(reg.execute(app, "/help") == AgentStatus.active);
    assert(reg.execute(app, "/debug") == AgentStatus.active);

    // Help lines are the verbatim strings
    auto help = reg.helpText();
    assert(help.canFind("   /help              Show this help message"));
    assert(help.canFind("   /quit, /q, /exit   Exit the agent"));
    assert(help.canFind("   /stop              Stop processing the currently active query"));
    assert(help.canFind("   /compact           Force compress the chat history"));
    assert(help.canFind("   /debug             Toggle verbose debug output"));
    assert(help.canFind("   /continue, /c      Continue processing the last query"));

    // Help ordering is (order asc, registration index asc): 10, 20, 30, 40, 140
    assert(help.indexOf("   /help") < help.indexOf("   /quit"));
    assert(help.indexOf("   /quit") < help.indexOf("   /stop"));
    assert(help.indexOf("   /stop") < help.indexOf("   /compact"));
    assert(help.indexOf("   /compact") < help.indexOf("   /debug"));

    // Restore process-global state mutated by the dispatched handlers
    // (/stop -> stopAgent(), /debug -> globalLogLevel) so later unittests
    // are not affected (order-dependent test pollution).
    clearStopAgent();
    logger.globalLogLevel = logger.LogLevel.info;
}
