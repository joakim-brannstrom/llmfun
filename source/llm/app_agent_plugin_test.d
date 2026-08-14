/// External-registration test (W2, §5.5): proves the startup command hook
/// works from OUTSIDE the `llm.app_agent` package.
///
/// This module deliberately lives outside the package — the module name is
/// `llm.app_agent_plugin_test` (NOT `llm.app_agent.*`), so the package
/// protection boundary is exercised: only the public plugin contract
/// (addStartupSlashCommand, SlashCommand, SlashArgMode, AgentStatus,
/// SlashCommandRegistry, AgentApp) is visible here. The handler is
/// registered via the startup hook and must appear in a subsequently
/// constructed AgentApp's registry.
///
/// NOTE: registration happens inside the unittest, NOT in a module
/// `static this()`: this file is compiled into the production binary too
/// (DUB compiles every source/ module), and a module constructor would
/// register "plugin-test" in real builds. The unittest body only runs
/// under `dub test`, keeping production clean while still proving the
/// hook's contract from outside the package.
module llm.app_agent_plugin_test;

import llm.app_agent : AgentApp;
import llm.app_agent.slash : AgentStatus, SlashArgMode, SlashCommand,
    SlashCommandRegistry, addStartupSlashCommand;
import llm.app_config : UserConfig;
import std.algorithm.searching : canFind;

unittest {
    // Unique command name: the startup list is global and append-only, so
    // the name must never collide with a built-in or another test module.
    // The handler returns AgentStatus.terminate — a registered command
    // dispatched to its handler is the ONLY path that yields terminate;
    // the unknown-command path yields active. No closure state needed.
    addStartupSlashCommand(SlashCommand("plugin-test", [], [
        SlashCommandRegistry.formatHelpLine("/plugin-test", "Test plugin registration")
    ], SlashArgMode.none, 900, (ref AgentApp app, string arg) => AgentStatus.terminate));

    auto app = AgentApp(UserConfig.AgentChatConfig.init);

    // The startup hook's command must be visible on the fresh instance.
    assert(app.slashCommands().helpText().canFind("   /plugin-test"),
            "startup hook command must appear in helpText()");

    // And it must dispatch to its handler (terminate proves the handler
    // ran; the unknown-command path would return active).
    assert(app.slashCommands().execute(app, "/plugin-test") == AgentStatus.terminate,
            "execute must dispatch the startup-hook command to its handler");

    // ArgMode.none exact-match rule applies to externally registered commands
    // too: an arg makes the command unknown (same path as in-package
    // commands, pinned by slash.d's stub tests).
    assert(app.slashCommands().execute(app, "/plugin-test x") == AgentStatus.active,
            "external command with an arg must take the unknown path");
}
