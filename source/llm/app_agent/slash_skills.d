/// Skill-management slash command: /skills.
module llm.app_agent.slash_skills;

import std.algorithm : filter;
import std.array : appender, array, empty, join;
import std.conv : text;
import std.format : format;
import std.functional : toDelegate;

import llm.app_agent; // AgentApp (cyclic package import, same pattern as slash.d)
import llm.app_agent.slash;
import llmfun_tui; // TuiChatMessageType_* (C binding)

import llm.config : effectiveAgentsMd;

package void registerSkillsCommands(ref SlashCommandRegistry registry) {
    auto ignore = registry.register(SlashCommand("skills", [],
            ["   /skills            List available skills"], SlashArgMode.none,
            150, toDelegate(&skillsHandler)));
    ignore = registry.register(SlashCommand("refresh-agent-md", [],
            ["   /refresh-agent-md  Force re-summarize AGENTS.md"],
            SlashArgMode.none, 160, toDelegate(&refreshAgentMdHandler)));
}

/// `/skills`: list the loaded skills.
private AgentStatus skillsHandler(ref AgentApp app, string arg) {
    app.sendChatMessage(formatSkillsList(app), TuiChatMessageType_Assistant);
    return AgentStatus.active;
}

private string formatSkillsList(ref AgentApp app) {
    if (app.skillManager_ is null) {
        return "No skill manager initialized.";
    }

    auto skills = app.skillManager_.getManifest();
    if (skills.empty) {
        return "No skills are currently loaded.";
    }

    auto alwaysApplyCount = skills.filter!(skill => skill.alwaysApply).array.length;
    auto lines = appender!(string[])();
    lines.put("Available skills:");
    lines.put("");

    foreach (skill; skills) {
        auto tag = skill.alwaysApply ? " [always-apply]" : "";
        auto desc = skill.description.length > 80
            ? skill.description[0 .. 77] ~ "..." : skill.description;
        lines.put(format("  %-25s %s", skill.name ~ tag, desc));
    }

    lines.put("");
    lines.put(i"$(skills.length) skills available, $(alwaysApplyCount) always-apply".text);
    return lines[].join("\n");
}

private AgentStatus refreshAgentMdHandler(ref AgentApp app, string arg) {
    const activeModel = app.llmConf.codeModels[app.llmConf.activeCodeModelIndex];
    if (!effectiveAgentsMd(app.llmConf, activeModel).enabled) {
        app.sendChatMessage("AGENTS.md handling is disabled; nothing to refresh",
                TuiChatMessageType_Assistant);
        return AgentStatus.active;
    }
    app.rebuildSystemPrompt(true); // forceRefresh: clear the cache, re-summarize
    return AgentStatus.active;
}

@("bare /skills dispatches to the handler with a null skill manager, and the help line renders")
unittest {
    import llm.app_config : UserConfig;
    import std.algorithm.searching : canFind;

    // AgentApp's constructor installs a blocked UiMessenger; the
    // unknown-command path dereferences uiMsg, so a real instance is needed.
    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    SlashCommandRegistry reg;
    registerSkillsCommands(reg);

    // SlashArgMode.none is asserted directly via the registry getter - the
    // primary guard. execute() with an arg would NOT discriminate here:
    // the handler ignores its arg, so both the handler path and the
    // unknown-command path return AgentStatus.active (the none-with-arg ->
    // unknown rule itself is pinned by slash.d's stub registry test).
    // Bare /skills IS dispatched as a smoke test: it exercises the handler,
    // formatSkillsList's null-manager early return, and the blocked
    // messenger (no agent_/rag dependency). Bare /refresh-agent-md is NOT:
    // its handler calls rebuildSystemPrompt, which needs a live agent_, rag
    // and skill manager.
    assert(reg.argModeOf("skills") == SlashArgMode.none);
    assert(reg.argModeOf("refresh-agent-md") == SlashArgMode.none);
    assert(reg.execute(app, "/skills") == AgentStatus.active);

    // Help lines are the verbatim strings
    auto help = reg.helpText();
    assert(help.canFind("   /skills            List available skills"));
    assert(help.canFind("   /refresh-agent-md  Force re-summarize AGENTS.md"));

    // Help ordering is (order asc, registration index asc): 150, 160
    import std.string : indexOf;

    assert(help.indexOf("   /skills") < help.indexOf("   /refresh-agent-md"));
}
