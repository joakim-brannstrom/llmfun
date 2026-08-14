/// Skill-management slash commands: /skills and /refresh-agent-md.
/// Also owns the command-only helpers (W9 split policy): `formatSkillsList`
/// and the refresh logic are module-private free functions taking
/// `ref AgentApp`, using the package-visible members (`skillManager_`,
/// `llmConf`, `rag`, `agent_`, `sendChatMessage`).
module llm.app_agent.slash_skills;

import std.algorithm : filter;
import std.array : appender, array, empty, join;
import std.conv : text;
import std.format : format;
import std.functional : toDelegate;

import llm.app_agent; // AgentApp (cyclic package import, same pattern as slash.d)
import llm.app_agent.slash;
import llm.agent_md : refreshAgentMd;
import llmfun_tui; // TuiChatMessageType_* (C binding)

package void registerSkillsCommands(ref SlashCommandRegistry registry) {
    auto ignore = registry.register(SlashCommand("skills", [],
            ["   /skills            List available skills"], SlashArgMode.none,
            150, toDelegate(&skillsHandler)));
    ignore = registry.register(SlashCommand("refresh-agent-md", [],
            ["   /refresh-agent-md  Force re-summarize AGENTS.md"],
            SlashArgMode.none, 160, toDelegate(&refreshAgentMdHandler)));
}

/// `/skills`: list the loaded skills (uses the `formatSkillsList` helper).
private AgentStatus skillsHandler(ref AgentApp app, string arg) {
    app.sendChatMessage(formatSkillsList(app), TuiChatMessageType_Assistant);
    return AgentStatus.active;
}

/// `/refresh-agent-md`: force re-summarize AGENTS.md (uses the
/// `handleRefreshAgentMd` helper, W9).
private AgentStatus refreshAgentMdHandler(ref AgentApp app, string arg) {
    handleRefreshAgentMd(app);
    return AgentStatus.active;
}

/// Format the loaded-skill list for `/skills`.
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

/// Refresh AGENTS.md: re-summarize and re-inject the summary into the agent's
/// system prompt.
private void handleRefreshAgentMd(ref AgentApp app) {
    app.sendChatMessage("assistant: Refreshing AGENTS.md... (summarizing, please wait)",
            TuiChatMessageType_Assistant);
    try {
        auto newState = refreshAgentMd(app.llmConf, app.rag);
        if (newState.isValid()) {
            app.agent_.setSystemPrompt(app.llmConf.getPrompt(skillManager: app.skillManager_, promptName: app
                    .llmConf.agentPrompt, addSkills: true, agentMdSummary: newState.summary));
            app.sendChatMessage("assistant: AGENTS.md refreshed successfully.\nSummary (%d chars):\n%s",
                    TuiChatMessageType_Assistant, newState.summary.length, newState.summary);
        } else {
            app.sendChatMessage("assistant: No AGENTS.md found in workarea, or refresh failed.",
                    TuiChatMessageType_Assistant);
        }
    } catch (Exception e) {
        app.sendChatMessage("assistant: Error refreshing AGENTS.md: %s",
                TuiChatMessageType_Assistant, e.msg);
    }
}

unittest {
    import llm.app_config : UserConfig;
    import std.algorithm.searching : canFind;
    import std.string : indexOf;

    // AgentApp's constructor installs a blocked UiMessenger (W5); the
    // unknown-command path dereferences uiMsg, so a real instance is needed.
    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    SlashCommandRegistry reg;
    registerSkillsCommands(reg);

    // SlashArgMode.none is asserted directly via the registry getter — the
    // primary guard. execute() with an arg would NOT discriminate here:
    // these handlers ignore their arg, so both the handler path and the
    // unknown-command path return AgentStatus.active (the none-with-arg →
    // unknown rule itself is pinned by slash.d's stub registry test).
    // Bare /skills IS dispatched as a smoke test: it exercises the handler,
    // formatSkillsList's null-manager early return, and the blocked
    // messenger (no agent_/rag dependency). Bare /refresh-agent-md is NOT:
    // its handler calls refreshAgentMd -> processAgentMd, which needs a
    // live rag and data dir.
    assert(reg.argModeOf("skills") == SlashArgMode.none);
    assert(reg.argModeOf("refresh-agent-md") == SlashArgMode.none);
    assert(reg.execute(app, "/skills") == AgentStatus.active);

    // Help lines are the §5.4 verbatim strings
    auto help = reg.helpText();
    assert(help.canFind("   /skills            List available skills"));
    assert(help.canFind("   /refresh-agent-md  Force re-summarize AGENTS.md"));

    // Help ordering is (order asc, registration index asc): 150, 160
    assert(help.indexOf("   /skills") < help.indexOf("   /refresh-agent-md"));
}
