/// Model-selection slash command: /model.
/// Self-contained model selection logic. ArgMode is `optional` — NOT `none`: the empty-arg form must
/// dispatch to the handler to list the available models.
module llm.app_agent.slash_model;

import std.array : empty;
import std.conv : text, to;
import std.exception : ifThrown;
import std.format : format;
import std.functional : toDelegate;
import std.string : strip;

import llm.app_agent; // AgentApp (cyclic package import, same pattern as slash.d)
import llm.app_agent.slash;
import llmfun_tui; // TuiChatMessageType_* (C binding)

package void registerModelCommands(ref SlashCommandRegistry registry) {
    auto ignore = registry.register(SlashCommand("model", [],
            [
                "   /model             List available models",
                "   /model <index>     Select model by index",
                "   /model <name>      Select model by exact name (case-insensitive)"
    ], SlashArgMode.optional, 110, toDelegate(&modelHandler)));
}

/// `/model [arg]`: no arg lists the available models; an index selects by
/// position, anything else is tried as an exact (case-insensitive) name.
/// The index is parsed into a signed `long` so a non-numeric arg (a model
/// name) routes to the name-match branch — an unsigned index would make
/// `idx >= 0` always true and leave `/model <name>` unreachable.
private AgentStatus modelHandler(ref AgentApp app, string arg) {
    arg = arg.strip;
    if (arg.empty) {
        auto m = "Available models:";
        foreach (i, model; app.llmConf.codeModels) {
            auto activeMarker = (i == cast(size_t) app.llmConf.activeCodeModelIndex) ? " [active]"
                : "";
            m ~= format("  %s  %s%s\n", i, model.modelName, activeMarker);
        }
        m ~= "Use /model <index> or /model <name> to switch.";
        app.sendChatMessage(m, TuiChatMessageType_Assistant);
    } else {
        const oldModel = app.llmConf.activeCodeModel.modelName;
        bool switched;
        long idx = ifThrown(arg.to!long, -1); // signed: -1 routes to name match
        if (idx >= 0) {
            switched = app.llmConf.selectModelByIndex(idx);
            if (!switched) {
                app.sendChatMessage("error: Invalid model index '%s'. Valid indices: 0-%s.",
                        TuiChatMessageType_Assistant, arg,
                        app.llmConf.codeModels.length == 0
                        ? "none" : text(app.llmConf.codeModels.length - 1));
            }
        } else {
            auto result = app.llmConf.selectModelByName(arg);
            switched = result.empty;
            if (!switched)
                app.sendChatMessage("failed to switch model: %s",
                        TuiChatMessageType_Assistant, result);
        }
        if (switched) {
            app.agent_.resetModel(app.llmConf.activeCodeModel());
            app.agent_.setStreamUpdate(app.makeStreamCallback);
            app.sendChatMessage("switched to model: %s\nAgent model reset: %s -> %s, context: %s",
                    TuiChatMessageType_Assistant,
                    app.llmConf.activeModelName(), oldModel,
                    app.agent_.modelName, app.agent_.modelContextSize);
        }
    }
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
    registerModelCommands(reg);

    // SlashArgMode.optional: bare /model dispatches to the handler (lists
    // models) — the empty-arg form must NOT take the unknown-command path.
    // The default LlmConfig has an empty codeModels list, so the list branch
    // runs without dereferencing agent_ (only created in run()).
    assert(reg.execute(app, "/model") == AgentStatus.active);
    assert(reg.execute(app, "/model ") == AgentStatus.active);

    // Help lines are the verbatim strings
    auto help = reg.helpText();
    assert(help.canFind("   /model             List available models"));
    assert(help.canFind("   /model <index>     Select model by index"));
    assert(help.canFind("   /model <name>      Select model by exact name (case-insensitive)"));

    // Help ordering: the three /model lines keep their registration order
    assert(help.indexOf("   /model             List available models") < help.indexOf(
            "   /model <index>"));
    assert(help.indexOf("   /model <index>") < help.indexOf("   /model <name>"));
}

unittest {
    // The signed `long idx` makes the name-match branch reachable. A
    // non-numeric arg (e.g. a model name) must take the `selectModelByName`
    // path — not the index path — and the failure branches must not
    // dereference `agent_` (null outside run()).
    import llm.app_config : UserConfig;
    import llm.config : CodeModelConfig;
    import llm.common.config : ServerConfig;

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    SlashCommandRegistry reg;
    registerModelCommands(reg);

    // Give the app a single model so `activeCodeModel()` works and
    // selectModelByIndex can be exercised.
    app.llmConf.codeModels = [
        CodeModelConfig(server: ServerConfig.init, modelName: "gpt-test")
    ];
    app.llmConf.activeCodeModelIndex = 0;

    // Non-numeric arg -> name-match branch. Unknown name -> error message, no
    // agent_ deref.
    assert(reg.execute(app, "/model nosuchmodel") == AgentStatus.active);

    // Numeric but out-of-range -> index branch error, no agent_ deref.
    assert(reg.execute(app, "/model 99") == AgentStatus.active);

    // Verify the name-match path is actually taken (not the index path):
    // "99" is numeric -> index branch; "nosuchmodel" is not numeric ->
    // name branch. Both stay on the !switched path (safe in tests).
}
