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

/// TurnID header round-trip: the high-water mark written by
/// `commitActiveSession` survives a `SessionStore.save` and seeds the counter
/// on reload (A5). Exercises the same chain commitActiveSession uses
/// (`extra["next_turn_id"]` -> save -> load -> `Chat.load`) without a live
/// Agent: `Chat` and `SessionStore` cover the whole seam.
unittest {
    import std.datetime : Clock;
    import std.file : mkdirRecurse;
    import std.format : format;
    import std.json : JSONValue, JSONType;

    import my.optional : hasValue, orElse;
    import my.path;
    import llm.chat : Chat, turnIdOf;
    import llm.session : SessionStore, SessionFile;

    // stdTime (100ns resolution) keeps two runs in the same second from
    // colliding on the temp dir.
    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/turnid_commit_%d_%d", now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        cleanupTurnIdTmp(tmpDir);

    auto store = new SessionStore(tmpDir.Path);
    auto meta = store.create();

    auto chat = Chat();
    chat.setSystemPrompt("sys");
    chat.addUserQuery("first question");
    chat.addUserQuery("second question");
    assert(chat.nextTurnId() == 2);

    // commitActiveSession's persistence seam: header high-water mark -> save,
    // with the same null_ guard as the production path (app_agent/package.d).
    if (meta.extra.type == JSONType.null_) {
        meta.extra = JSONValue.emptyObject;
    }
    meta.extra["next_turn_id"] = chat.nextTurnId();
    meta = store.save(meta.id, meta, chat.toSaveJson());

    auto sfOpt = store.load(meta.id);
    assert(hasValue(sfOpt));
    auto sf = orElse(sfOpt, SessionFile());
    assert(sf.doc["next_turn_id"].integer == 2, "header must persist the counter");

    // Reload continues the session's own sequence (I-1: no +1 seeding).
    Chat reloaded;
    reloaded.load(sf.doc);
    assert(reloaded.nextTurnId() == 2);
    reloaded.addUserQuery("third question");
    assert(reloaded.currentTurnId() == 3);
    foreach (i; 1 .. reloaded.getMessages.length) {
        assert(turnIdOf(reloaded.getMessages[i]) > 0, "loaded messages must be stamped");
    }
}

/// Removes a turn-id test session dir; failures only log (test hygiene).
private void cleanupTurnIdTmp(string dir) {
    import std.exception : collectException;
    import std.file : rmdirRecurse;

    import logger = std.logger;

    try {
        rmdirRecurse(dir);
    } catch (Exception e) {
        logger.trace(e.msg).collectException;
    }
}
