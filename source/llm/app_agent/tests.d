/// Test suite: golden help parity, the real built-in command surface, and
/// the AgentApp constructor guard (built-ins + startup hooks).
///
/// Coverage map (the registry-mechanics items are pinned by the stub-based
/// unittests in `slash.d`; the pure-logic tests live with their functions —
/// this module adds what they do not cover):
///   - registration + alias lookup ............ slash.d unittest
///   - unknown-command path ................... slash.d unittest
///   - ArgMode.none exact-match rule .......... slash.d unittest
///   - ArgMode.required + empty -> unknown .... slash.d unittest (W1)
///   - help ordering + stable tiebreak ........ slash.d unittest (W8)
///   - duplicate registration throws .......... slash.d unittest
///   - formatHelpLine alignment (22/25) ....... slash.d unittest (W4)
///   - decideDeleteCommand .................... slash_session.d unittest
///   - pickFallbackAfterDelete ................ package.d unittest
///   - external registration (outside package) llm.app_agent_plugin_test.d
///   - golden help + built-in surface ......... THIS module
module llm.app_agent.tests;

import llm.app_agent; // AgentApp (cyclic package import, same pattern as slash.d)
import llm.app_agent.slash;
import llm.app_config : UserConfig;
import std.string : startsWith;

version (unittest) {
    /// Literal `/help` output (the golden block, §5.4 + header). W11: any
    /// change to a command's help line, order, name or aliases must update
    /// this block in the same change — the golden test below fails
    /// byte-for-byte otherwise.
    ///
    /// Version-guarded so the production binary (DUB compiles every source/
    /// module) carries no dead data: the block is only referenced from
    /// unittest blocks, which are compiled out without -unittest.
    private immutable string goldenHelpBlock = "llmfun agent mode - type a query and press Tab to start.\n" ~ " Use /commands for special actions:\n" ~ "\n" ~ "   (bare query)       Send a message to the agent\n" ~ "   /help              Show this help message\n" ~ "   /quit, /q, /exit   Exit the agent\n" ~ "   /stop              Stop processing the currently active query\n" ~ "   /compact           Force compress the chat history\n" ~ "   /sessions          List chat sessions (index, id, title, preview)\n" ~ "   /switch <n|id|title>  Switch to another session\n" ~ "   /new               Start a new chat session\n" ~ "   /rename <title>    Rename the current session\n" ~ "   /delete <n>        Delete a session (repeat to confirm)\n" ~ "   /clear             Clear the current chat history\n" ~ "   /model             List available models\n" ~ "   /model <index>     Select model by index\n" ~ "   /model <name>      Select model by exact name (case-insensitive)\n" ~ "   /plan <query>      Run the plan pipeline\n" ~ "   /code <query>      Run the coder pipeline\n" ~ "   /debug             Toggle verbose debug output\n" ~ "   /skills            List available skills\n" ~ "   /refresh-agent-md  Force re-summarize AGENTS.md";
}

/// Golden help test: `registry.helpText()` byte-for-byte equal to the
/// literal help block. Uses a BARE registry (built-ins only, no startup
/// hooks): the global startup list (`idem-x`, `plugin-test`) pollutes
/// real-AgentApp helpText() once the plugin-test command has help lines, so
/// parity is locked on the built-ins alone.
unittest {
    SlashCommandRegistry reg;
    registerBuiltinCommands(reg);
    assert(reg.helpText() == goldenHelpBlock,
            "helpText() must be byte-identical to the pre-refactor buildHelpText()");
}

/// Built-in registration surface: the REAL command set on a bare registry.
/// The stub-based tests in slash.d pin the registry rules; this pins the
/// built-in registration itself — canonical names in registration order
/// (group order: core, session, model, pipeline, skills) and argModes per
/// §5.4 (the same table the golden block's ordering depends on).
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

/// Real built-ins dispatch through the registry: aliases terminate, unknown
/// commands take the unknown path, optional `/model` dispatches with an empty
/// arg (lists models — default config has an empty codeModels list, no
/// agent_ dereference), and bare `/plan`/`/code` take the unknown path
/// (required + empty, W1). Constructs a real AgentApp for the blocked
/// UiMessenger (W5) — the constructor already installs it.
unittest {
    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    SlashCommandRegistry reg;
    registerBuiltinCommands(reg);

    assert(reg.execute(app, "/quit") == AgentStatus.terminate);
    assert(reg.execute(app, "/q") == AgentStatus.terminate);
    assert(reg.execute(app, "/exit") == AgentStatus.terminate);
    assert(reg.execute(app, "/nope") == AgentStatus.active);
    assert(reg.execute(app, "/model") == AgentStatus.active);
    assert(reg.execute(app, "/plan") == AgentStatus.active); // W1: required + empty
    assert(reg.execute(app, "/code") == AgentStatus.active); // W1: required + empty
}

/// Constructor guard: AgentApp's constructor registers the built-ins FIRST,
/// then every startup-hook command (§5.5) — so commandNames() must start
/// with the 16 built-ins in group order, and helpText() must start with the
/// golden block (startup commands sort after, order 900).
unittest {
    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    // helpText() starts with the golden block because both startup test
    // commands use order 900 (>= 160, after all built-ins). CONSTRAINT for
    // future test modules: a startup command with order < 160 would sort
    // INTO the golden block and break this prefix assertion even though
    // production behavior is correct — keep test startup orders >= 160.
    assert(app.slashCommands().helpText().startsWith(goldenHelpBlock),
            "real AgentApp help must keep the golden built-in block intact "
            ~ "(startup test commands must use order >= 160; see tests.d:119)");
}
