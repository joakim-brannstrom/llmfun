/// Integration test suite for slash commands.
module llm.app_agent.tests;

import llm.app_agent;
import llm.app_agent.slash;
import llm.app_config : UserConfig;
import std.string : startsWith;

// Built-in registration surface: the REAL command set on a bare registry.
// Checks that the modules register as expected.
unittest {
    SlashCommandRegistry reg;
    registerBuiltinCommands(reg);

    assert(reg.argModeOf("help") == SlashArgMode.none);
    assert(reg.argModeOf("quit") == SlashArgMode.none);
    assert(reg.argModeOf("stop") == SlashArgMode.none);
    assert(reg.argModeOf("compact") == SlashArgMode.none);
    assert(reg.argModeOf("debug") == SlashArgMode.none);
    assert(reg.argModeOf("sessions") == SlashArgMode.none);
    assert(reg.argModeOf("switch") == SlashArgMode.required);
    assert(reg.argModeOf("new") == SlashArgMode.none);
    assert(reg.argModeOf("rename") == SlashArgMode.required);
    assert(reg.argModeOf("delete") == SlashArgMode.optional);
    assert(reg.argModeOf("clear") == SlashArgMode.none);
    assert(reg.argModeOf("model") == SlashArgMode.optional);
    assert(reg.argModeOf("plan") == SlashArgMode.required);
    assert(reg.argModeOf("code") == SlashArgMode.required);
    assert(reg.argModeOf("skills") == SlashArgMode.none);
    assert(reg.argModeOf("refresh-agent-md") == SlashArgMode.none);
}

// Real built-ins dispatch through the registry.
// - aliases terminate, unknown commands take the unknown path,
// - optional `/model` dispatches with an empty arg (lists models - default
//   config has an empty codeModels list, no agent_ dereference),
// - bare `/plan`/`/code` take the unknown path.
//
// Constructs a real AgentApp for the blocked UiMessenger.
unittest {
    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    SlashCommandRegistry reg;
    registerBuiltinCommands(reg);

    assert(reg.execute(app, "/quit") == AgentStatus.terminate);
    assert(reg.execute(app, "/q") == AgentStatus.terminate);
    assert(reg.execute(app, "/exit") == AgentStatus.terminate);
    assert(reg.execute(app, "/nope") == AgentStatus.active);
    assert(reg.execute(app, "/model") == AgentStatus.active);
    assert(reg.execute(app, "/plan") == AgentStatus.active);
    assert(reg.execute(app, "/code") == AgentStatus.active);
}
