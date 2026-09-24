/// Integration tests for AgentApp: slash dispatch, session persistence, and sidebar actions.
module llm.app_agent.tests;

import llm.app_agent;
import llm.app_agent.slash;
import llm.app_config : UserConfig, userToLlmConfig, createRag;
import std.string : startsWith, strip, join;
import llm.session : SessionId, SessionMeta, SessionFile, SessionStore, isValidId;
import my.path : Path, AbsolutePath;
import my.optional : Optional, hasValue, orElse;
import std.concurrency;
import llm.config;
import logger = std.logger;
import std.algorithm;
import std.array : empty, array, appender;
import std.conv : to, text;
import std.exception : collectException;
import std.datetime : Clock, SysTime, DateTime, UTC, dur;
import std.format : format;
import std.json : JSONType, JSONValue;
import std.sumtype : match;
import llm.agent;
import llm.agent_md;
import llm.app_agent.ui;
import llm.chat;
import llm.config : RagConfig;
import llm.memory;
import llm.metric.monitor : MetricMonitor;
import llm.query;
import llm.rag.dialogue_index : DialogueIndex;
import llm.rag.dialogue_worker : DiDegraded;
import llm.rag.reasoning_index : ReasoningIndex, loadReasoningPrompt;
import llm.rag.rag : RAG;
import llm.skill;
import llm.tui;
import llm.types : ServerStat, IStreamCallback;
import llm.utility;
import llmfun_tui;

version (unittest) {
    /// Minimal headless LlmConfig for constructing a real Agent in tests: one code model (never contacted) plus a temp summary prompt file. dataDir is intentionally NOT created so dispose()'s saveState() is a no-op - the tests never write a state.json.
    ///
    /// Coupling invariants: the Agent is built with null SkillManager/MetricMonitor/RAG (tolerated by the constructor) and a model name that is never contacted; any future Agent constructor change must keep this helper compiling and passing.
    package LlmConfig testLlmConfig(string tmpDir) {
        import std.file : mkdirRecurse, write;
        import std.path : buildPath;
        import llm.common.config : ServerConfig;

        auto promptDir = buildPath(tmpDir, "prompt");
        mkdirRecurse(promptDir);
        write(buildPath(promptDir, "SUMMARY.md"), "test summary prompt");

        LlmConfig cfg;
        cfg.codeModels = [
            CodeModelConfig(server: ServerConfig.init, modelName: "test-model")
        ];
        cfg.activeCodeModelIndex = 0;
        cfg.promptDir = [promptDir.Path];
        cfg.dataDir = buildPath(tmpDir, "no-state").Path;
        return cfg;
    }
}

// Built-in registration surface: the REAL command set on a bare registry. Checks that the modules register as expected.
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
}

// Real built-ins dispatch through the registry.
// - aliases terminate, unknown commands take the unknown path,
// - optional `/model` dispatches with an empty arg (lists models - default config has an empty codeModels list, no agent_ dereference),
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

// TurnID header round-trip: the high-water mark written by `commitActiveSession` survives a `SessionStore.save` and seeds the counter on reload. Exercises the same chain commitActiveSession uses (`extra["next_turn_id"]` -> save -> load -> `Chat.load`) without a live Agent: `Chat` and `SessionStore` cover the whole seam."
@("TurnID header round-trip: high-water mark")
unittest {
    import std.datetime : Clock;
    import std.file : mkdirRecurse;
    import std.format : format;
    import std.json : JSONValue, JSONType;

    import my.optional : hasValue, orElse;
    import my.path;
    import llm.chat : Chat, turnIdOf;
    import llm.session : SessionStore, SessionFile;

    // stdTime (100ns resolution) keeps two runs in the same second from colliding on the temp dir.
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

    // commitActiveSession's persistence seam: header high-water mark -> save, with the same null_ guard as the production path (app_agent/package.d).
    if (meta.extra.type == JSONType.null_) {
        meta.extra = JSONValue.emptyObject;
    }
    meta.extra["next_turn_id"] = chat.nextTurnId();
    meta = store.save(meta.id, meta, chat.toSaveJson());

    auto sfOpt = store.load(meta.id);
    assert(hasValue(sfOpt));
    auto sf = orElse(sfOpt, SessionFile());
    assert(sf.doc["next_turn_id"].integer == 2, "header must persist the counter");

    // Reload continues the session's own sequence.
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

// Regression: every agent turn that appends persisted content (tool traffic, the final taskDone answer) must be committed by processResult's per-step commit - a turn after the first commit of a run used to be silently dropped on session switch / app restart
@("every agent turn that appends persisted content must be committed")
unittest {
    import std.file : exists, mkdirRecurse, rmdirRecurse;
    import std.json : JSONType, JSONValue;
    import std.path : buildPath;

    import my.optional : orElse;
    import my.path : Path;
    import std.sumtype : match;
    import llm.agent : Agent;
    import llm.chat;
    import llm.session : SessionFile, SessionStore;
    import llm.types : ProcessResult;
    import my.filter : ReFilter;

    auto tmpDir = buildPath("llmfun_test", "app_agent_process_result_dirty");
    if (exists(tmpDir))
        rmdirRecurse(tmpDir);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto cfg = testLlmConfig(tmpDir);
    auto store = new SessionStore(buildPath(tmpDir, "chat").Path);

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.llmConf = cfg;
    app.sessionStore = store;
    // AgentApp's ctor leaves agent_ null (created lazily in run(): private int run, `agent_ = new Agent(...)`); tests that drive the Agent assign it manually, as the production run flow does and the sibling tests below.
    app.agent_ = new Agent("main", cfg, null, null, null, ReFilter.init);

    // The session as it looks after a committed query: one user message, persisted directly through the store, then activated. Loading resets the app's dirty flag (activateSession, before its replay loop), so the turn below starts from a clean, persisted state.
    auto meta = store.create();
    JSONValue doc;
    JSONValue userMsg;
    userMsg["role"] = "user";
    userMsg["content"] = "what is 2+2?";
    doc["messages"] = JSONValue([userMsg]);
    app.activeSession = store.save(meta.id, meta, doc);
    app.switchToSession(meta.id);

    // Turn 2 - appends ONLY agent-layer content, exactly as production does: handleToolCalls adds the taskDone ToolMessage (with the final answer in save_data) and its ToolResponse (agent), process() reports the appended slice through ProcessResult.chat (rval.chat = chat.lastResponses), and runToCompletion hands it to processResult as the step hook (the &this.processResult delegate). Before the fix this turn was never persisted: the flag stayed false and the commit was a no-op, so a session switch/restart lost the final answer.
    JSONValue call;
    call["id"] = "call_1";
    call["type"] = "function";
    JSONValue fn;
    fn["name"] = "taskDone";
    fn["arguments"] = "{}";
    call["function"] = fn;

    JSONValue sd;
    sd["taskDoneAnswer"] = JSONValue("The answer is 4.");

    app.agent_.chat.add(ToolMessage(null, JSONValue([call]), JSONValue.init, sd));
    app.agent_.chat.add(ToolResponse("4", "call_1", "taskDone", true));

    ProcessResult turn;
    turn.status = ProcessResult.Status.ok;
    turn.chat = app.agent_.chat.lastResponses;
    app.agent_.chat.resetResponseIndex();

    app.processResult(turn); // sets the dirty flag, then commits

    auto saved = orElse(store.load(app.activeSession.id), SessionFile());
    bool hasFinalAnswer;
    foreach (entry; saved.doc["messages"].array) {
        if (entry.type == JSONType.object && ("save_data" in entry.object) !is null
                && entry.object["save_data"].type == JSONType.object
                && ("taskDoneAnswer" in entry.object["save_data"].object) !is null)
            hasFinalAnswer = true;
    }
    assert(hasFinalAnswer,
            "the final-answer turn must be persisted: a save_data.taskDoneAnswer message must be in the session file");

    // Replaying the persisted doc yields the final-answer message (the replayed chat renders the green FinalAnswer header).
    Chat replayed;
    replayed.load(saved.doc);
    bool sawFinalAnswer;
    foreach (msg; replayed.getMessages()) {
        msg.match!((Message _) {}, (ToolMessage t) {
            if (t.isFinalAnswer)
                sawFinalAnswer = true;
        }, (ToolResponse _) {}, (VisionMessage _) {});
    }
    assert(sawFinalAnswer, "the persisted final answer must load back as a ToolMessage");
}

// Regression: the final-answer turn of a two-turn run. Turn 1 is a user query committed through the app-level submit primitives; turn 2 resets the flag and appends ONLY agent tool traffic (the taskDone ToolMessage carrying the final answer) - its persistence depends entirely on processResult's per-turn dirty-flag set, because the submit path never sets the flag again. That turn used to be silently dropped on session switch / app restart
@("final-answer turn after a committed user query is persisted")
unittest {
    import std.file : exists, mkdirRecurse, rmdirRecurse;
    import std.json : JSONType, JSONValue;
    import std.path : buildPath;

    import my.optional : orElse;
    import my.path : Path;
    import std.sumtype : match;
    import llm.agent : Agent;
    import llm.chat;
    import llm.session : SessionFile, SessionStore;
    import llm.types : ProcessResult;
    import my.filter : ReFilter;

    auto tmpDir = buildPath("llmfun_test", "app_agent_final_answer_persist");
    if (exists(tmpDir))
        rmdirRecurse(tmpDir);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto cfg = testLlmConfig(tmpDir);
    auto store = new SessionStore(buildPath(tmpDir, "chat").Path);

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.llmConf = cfg;
    app.sessionStore = store;
    // AgentApp's ctor leaves agent_ null (created lazily in run(): private int run, `agent_ = new Agent(...)`); tests that drive the Agent assign it manually, as the production run flow does and the sibling tests below.
    app.agent_ = new Agent("main", cfg, null, null, null, ReFilter.init);

    // Establish the active session the way production does: create() writes the file with messages: [], then switchToSession activates it (clean loaded chat, flag false, activeSession set). Without this the turn commits below would write under a default (empty) SessionId.
    auto meta = store.create();
    app.switchToSession(meta.id);

    // Turn 1: a user query committed through the app-level submit path - addUserQuery + flag + commit, the primitives runAgent uses (agent_.addUserQuery then chatDirty = true).
    app.agent_.addUserQuery("hello");
    app.chatDirty = true;
    app.commitActiveSession();

    // Turn 2: flag cleared (as after turn 1's commit), then the agent turn appends ONLY agent-layer content, exactly as production does: handleToolCalls adds the taskDone ToolMessage (with the final answer in save_data) and its ToolResponse (agent), process() reports the appended slice through ProcessResult.chat (rval.chat = chat.lastResponses), and runToCompletion hands it to processResult as the step hook (the &this.processResult delegate). (Turn 1 bypasses process(), so prevIndex stays 0 and turn 2's lastResponses slice also replays the user message - benign: printUser:false -> trace only; persistence serializes the full history regardless.) Before the fix this turn was never persisted: the flag stayed false and the commit was a no-op, so a session switch/restart lost the final answer. (The task snippet's direct r.chat array is reshaped into the sibling test's append + lastResponses pattern - the production-exact seam; processResult only reads result.chat.)
    app.chatDirty = false;

    JSONValue call;
    call["id"] = "call_1";
    call["type"] = "function";
    JSONValue fn;
    fn["name"] = "taskDone";
    fn["arguments"] = "{}";
    call["function"] = fn;

    JSONValue sd;
    sd["taskDoneAnswer"] = JSONValue("The answer is 4.");

    app.agent_.chat.add(ToolMessage(null, JSONValue([call]), JSONValue.init, sd));
    app.agent_.chat.add(ToolResponse("4", "call_1", "taskDone", true));

    ProcessResult r;
    r.status = ProcessResult.Status.ok;
    r.chat = app.agent_.chat.lastResponses;
    app.agent_.chat.resetResponseIndex();

    app.processResult(r); // sets the dirty flag, then commits

    assert(app.chatDirty == false, "turn-2 commit must clear the dirty flag");

    // Both turns are in the persisted file: turn 1's user query and turn 2's final answer.
    auto saved = orElse(store.load(app.activeSession.id), SessionFile());
    bool hasUserHello, hasFinalAnswer;
    foreach (entry; saved.doc["messages"].array) {
        if (entry.type != JSONType.object)
            continue;
        if (("role" in entry.object) !is null
                && entry.object["role"].type == JSONType.string
                && entry.object["role"].str == "user" && ("content" in entry.object) !is null
                && entry.object["content"].type == JSONType.string
                && entry.object["content"].str == "hello")
            hasUserHello = true;
        if (("save_data" in entry.object) !is null
                && entry.object["save_data"].type == JSONType.object
                && ("taskDoneAnswer" in entry.object["save_data"].object) !is null)
            hasFinalAnswer = true;
    }
    assert(hasUserHello, "turn 1's user query must still be in the session file");
    assert(hasFinalAnswer,
            "the final-answer turn must be persisted: a save_data.taskDoneAnswer message must be in the session file");

    // Replaying the persisted doc yields the final-answer message (the replayed chat renders the green FinalAnswer header).
    Chat replayed;
    replayed.load(saved.doc);
    bool sawFinalAnswer;
    foreach (msg; replayed.getMessages()) {
        msg.match!((Message _) {}, (ToolMessage t) {
            if (t.isFinalAnswer)
                sawFinalAnswer = true;
        }, (ToolResponse _) {}, (VisionMessage _) {});
    }
    assert(sawFinalAnswer, "the persisted final answer must load back as a ToolMessage");
}

/// Regression: a session switch re-applies the cached startup system prompt activateSession clears the chat with `Chat.clear`, which keeps `history[0]`, and loaded docs carry no system entries, so without the re-apply the previous chat's prompt would survive the switch.
@("session switch re-applies the startup system prompt")
unittest {
    import std.file : exists, mkdirRecurse, rmdirRecurse;
    import std.path : buildPath;
    import std.sumtype : match;

    import llm.agent : Agent;
    import llm.chat : Chat, Message, Role, ToolMessage, ToolResponse, VisionMessage;
    import llm.session : SessionStore;
    import my.filter : ReFilter;
    import my.path : Path;

    // The system prompt at `history[0]` of a chat, or null when the first entry is not a system Message (loaded docs carry none before startup).
    auto systemPromptOf(Chat.MessageT[] msgs) {
        string found;
        if (msgs.length == 0)
            return found;
        msgs[0].match!((Message a) {
            if (a.role == Role.system)
                found = a.content;
        }, (ToolMessage _) {}, (ToolResponse _) {}, (VisionMessage _) {});
        return found;
    }

    auto tmpDir = buildPath("llmfun_test", "app_agent_system_prompt_switch");
    if (exists(tmpDir))
        rmdirRecurse(tmpDir);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto cfg = testLlmConfig(tmpDir);
    auto store = new SessionStore(buildPath(tmpDir, "chat").Path);

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.llmConf = cfg;
    app.sessionStore = store;
    // AgentApp's ctor leaves agent_ null (created lazily in run()); tests that drive the Agent assign it manually, as the sibling tests do.
    app.agent_ = new Agent("main", cfg, null, null, null, ReFilter.init);

    // Startup simulation: setupSession's setSystemPrompt call site is the only place that assigns the cache, so the test seeds cache and chat the same way startup does (setupSession itself is private and needs a full run()).
    const startupPrompt = "startup system prompt";
    app.systemPrompt_ = startupPrompt;
    app.agent_.setSystemPrompt(startupPrompt);

    // Session A, activated the production way (switchToSession): the loaded chat keeps the startup prompt at history[0] and the flag stays false.
    auto metaA = store.create();
    app.switchToSession(metaA.id);
    assert(app.agent_.chat.getMessages().length == 1, "setup: session A loaded empty");
    assert(systemPromptOf(app.agent_.chat.getMessages()) == startupPrompt,
            "setup: the activated chat keeps the startup prompt");
    assert(app.chatDirty == false, "setup: activation must keep the chat clean");

    // The chat drifts off the startup prompt before the switch - standing in for whatever left a different prompt at history[0] (Chat.load only sets one when history is empty). Without the fix the stale prompt survives.
    const stalePrompt = "stale session prompt";
    app.agent_.setSystemPrompt(stalePrompt);
    assert(systemPromptOf(app.agent_.chat.getMessages()) == stalePrompt,
            "setup: the prompt at history[0] must be stale before the switch");

    // Switch A -> B: the fix restores the startup prompt; without it the stale prompt survives the switch.
    auto metaB = store.create();
    app.switchToSession(metaB.id);
    assert(systemPromptOf(app.agent_.chat.getMessages()) == startupPrompt,
            "activating session B must restore the startup prompt");

    // ... and B -> A restores it again (the full A->B->A criterion).
    app.switchToSession(metaA.id);
    assert(systemPromptOf(app.agent_.chat.getMessages()) == startupPrompt,
            "activating session A again must restore the startup prompt");

    // Exactly one system message: the re-apply must not duplicate prompts.
    size_t systemCount;
    foreach (m; app.agent_.chat.getMessages())
        m.match!((Message a) {
            if (a.role == Role.system)
                systemCount++;
        }, (ToolMessage _) {}, (ToolResponse _) {}, (VisionMessage _) {});
    assert(systemCount == 1, "the re-apply must not add a second system message");

    // /clear keeps the prompt: driven through the REAL dispatch path - the registry routes the command to Agent.clearHistory() -> chat.clear, the slash_session.d tests cover /clear end-to-end; here the in-session wipe must not drop the prompt.
    import llm.app_agent.slash : AgentStatus;

    assert(app.slashCommands_.execute(app, "/clear") == AgentStatus.active,
            "/clear must dispatch as an in-session command");
    assert(systemPromptOf(app.agent_.chat.getMessages()) == startupPrompt,
            "/clear keeps the system prompt");
}

@("pickFallbackAfterDelete selects the most recently updated session")
unittest {
    SessionMeta a, b, c;
    a.id = SessionId("20260618-153045-a1b2");
    a.updatedAt = 100;
    b.id = SessionId("20260618-153045-b3c4");
    b.updatedAt = 300;
    c.id = SessionId("20260618-153045-c5d6");
    c.updatedAt = 200;

    // Input not sorted: the helper must scan for the maximum updatedAt
    auto remaining = [a, b, c];
    assert(AgentApp.pickFallbackAfterDelete(remaining) == b.id,
            "most recently updated session should be picked");

    // Single remaining session wins
    auto single = [a];
    assert(AgentApp.pickFallbackAfterDelete(single) == a.id);

    // Empty list -> caller creates a fresh session
    SessionMeta[] none;
    assert(AgentApp.pickFallbackAfterDelete(none) == SessionId.init);

    // Ties keep the first occurrence (deterministic)
    SessionMeta d;
    d.id = SessionId("20260618-153045-d7e8");
    d.updatedAt = 300;
    auto tie = [b, d];
    assert(AgentApp.pickFallbackAfterDelete(tie) == b.id,
            "ties should resolve deterministically to the first occurrence");
}

@("dispatcher-level pending-delete parity")

unittest {
    // The most safety-critical behavior of the dispatcher: a stale `pendingDeleteId` must never make the next `/delete <n>` confirm-delete without re-prompting. runAgent's parity guard (`query.startsWith("/delete") && !slashCommands_.isRegistered(query)`) clears /delete-prefixed NON-commands; the top rule clears everything else; registered /delete-prefixed commands are left to their handlers. The registry's unknown path does NOT clear pending state.
    import llm.app_config : UserConfig;

    // Blocked UiMessenger - the unknown path writes via writeln, never a null uiMsg dereference.
    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    auto stale = SessionId("stale-id");

    // Registered /delete-prefixed command: the dispatcher must NOT clear — the handler owns the delete state machine. deleteprobe's handler does not touch pendingDeleteId, so any dispatcher clear would be visible.
    app.registerSlashCommand(SlashCommand("deleteprobe", [], [],
            SlashArgMode.none, 900, (ref AgentApp a, string arg) => AgentStatus.active));
    app.pendingDeleteId = stale;
    assert(app.runAgent("/deleteprobe") == AgentStatus.active);
    assert(app.pendingDeleteId == stale,
            "dispatcher must not clear pending for registered /delete-prefixed commands");

    // `/deletefoo` (unregistered /delete-prefixed typo): the registry's unknown path does not clear pending, so runAgent's guard is the ONLY clearing path. Without it the next `/delete 3` would confirm-delete immediately instead of re-prompting.
    app.pendingDeleteId = stale;
    assert(app.runAgent("/deletefoo") == AgentStatus.active);
    assert(app.pendingDeleteId == SessionId.init,
            "/delete-prefixed non-commands must clear a stale confirmation");

    // Non-/delete unknown: cleared by the top rule, not the registry.
    app.pendingDeleteId = stale;
    assert(app.runAgent("/nope") == AgentStatus.active);
    assert(app.pendingDeleteId == SessionId.init, "non-/delete inputs clear via the top rule");

    // `/delete` (registered, handler-owned): dispatch still reaches the handler; the empty-arg error path clears pending.
    app.pendingDeleteId = stale;
    assert(app.runAgent("/delete") == AgentStatus.active);
    assert(app.pendingDeleteId == SessionId.init);
}

// --- Test: sidebar snapshot mapping (SessionMeta[] -> UiSessionItem[]) ---

unittest {
    SessionMeta a, b;
    a.id = SessionId("20260618-153045-a1b2");
    a.title = "Alpha";
    a.preview = "prev a";
    a.messageCount = 3;
    a.userMessageCount = 2;
    b.id = SessionId("20260618-153045-b3c4");
    b.title = "Beta";
    b.preview = "prev b";
    b.messageCount = 5;
    b.userMessageCount = 1;

    // Active marker follows the activeId argument; order is preserved.
    auto items = AgentApp.mapSessionItems([a, b], b.id);
    assert(items.length == 2);
    assert(items[0].id == a.id);
    assert(items[0].title == "Alpha");
    assert(items[0].preview == "prev a");
    assert(items[0].messageCount == 3);
    assert(!items[0].isActive);
    assert(items[1].id == b.id);
    assert(items[1].title == "Beta");
    assert(items[1].messageCount == 5);
    assert(items[1].isActive);

    // Empty input -> empty snapshot; no active session -> no active row.
    assert(AgentApp.mapSessionItems([], SessionId.init).length == 0);
    auto noActive = AgentApp.mapSessionItems([a], SessionId.init);
    assert(noActive.length == 1 && !noActive[0].isActive);
}

@("Test: sidebar invalid-id rejection for each action type")

unittest {
    import llm.app_config : UserConfig;

    // No store is configured: reaching the store would crash the test, so a green run proves rejection happens BEFORE any store access.
    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    auto bad = SessionId("bad-id");

    app.doSidebarSelect(bad);
    assert(app.pendingDeleteId == SessionId.init, "Select clears pending delete (A5)");

    app.doSidebarRename(bad, "x");
    assert(app.pendingDeleteId == SessionId.init, "Rename clears pending delete (A5)");

    app.doSidebarDelete(bad);
    assert(app.pendingDeleteId == SessionId.init, "Delete clears pending delete (A5)");
}

// sidebar rename input validation (empty title rejected, long non-empty title accepted - no length cap, matches /rename)
@("sidebar rename input validation")
unittest {
    import std.file : exists, rmdirRecurse, mkdirRecurse;
    import std.path : buildPath;
    import std.array : replicate;

    auto tmpDir = buildPath("llmfun_test", "app_agent_rename_validate");
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto store = new SessionStore(tmpDir.Path);
    auto meta = store.create();
    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.sessionStore = store;
    app.activeSession = meta;

    // Whitespace-only title: rejected, store unchanged.
    app.doSidebarRename(meta.id, "   ");
    assert(orElse(store.load(meta.id), SessionFile()).meta.title == meta.title,
            "empty title must not change the stored title");

    // Long non-empty title: accepted (no length cap anywhere).
    auto longTitle = "T".replicate(300);
    app.doSidebarRename(meta.id, longTitle);
    assert(orElse(store.load(meta.id), SessionFile()).meta.title == longTitle,
            "long non-empty title must be accepted");
    assert(app.activeSession.title == longTitle,
            "active meta must refresh when the renamed id is active");
}

// sidebar rename-none error handling (unknown id, corrupt file:
// error emitted, active meta unchanged, list still refreshed)
@("sidebar rename-none error handling")
unittest {
    import std.file : exists, rmdirRecurse, mkdirRecurse, write;
    import std.path : buildPath;

    auto tmpDir = buildPath("llmfun_test", "app_agent_rename_none");
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto store = new SessionStore(tmpDir.Path);
    auto meta = store.create();
    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.sessionStore = store;
    app.activeSession = meta;
    // Active UiMessenger pointed at this thread: the handler's error chat message and the sendSessionList() refresh both land in this thread's own mailbox, so the test can observe "error emitted" and "list still refreshed" without spawning a thread.
    app.uiMsg = new UiMessenger(thisTid, false);
    app.uiTid = thisTid;

    // Unknown id (valid format, no file): rename -> none -> error message, active meta unchanged, list still refreshed.
    auto unknown = SessionId("20260618-153045-ffff");
    app.doSidebarRename(unknown, "New title");
    bool gotError = false;
    receiveTimeout(dur!"seconds"(1), (UiChatMessage m) {
        gotError = m.msg == "error: Failed to rename session 'ffff'.";
    });
    assert(gotError, "rename-none must emit the error chat message");
    assert(app.activeSession.id == meta.id, "rename-none must keep the active meta");
    assert(app.activeSession.title == meta.title);
    bool gotList = false;
    receiveTimeout(dur!"seconds"(1), (immutable UiSessionList l) {
        gotList = true;
        assert(l.items.length == 1, "list still refreshed on rename-none");
    });
    assert(gotList, "rename-none must still refresh the sidebar list");

    // Corrupt file (valid id, garbage JSON): same none path.
    write(buildPath(tmpDir, "20260618-153046-0bad.json"), "{ not json !!!");
    auto corrupt = SessionId("20260618-153046-0bad");
    app.doSidebarRename(corrupt, "New title");
    gotError = false;
    receiveTimeout(dur!"seconds"(1), (UiChatMessage m) {
        gotError = m.msg == "error: Failed to rename session '0bad'.";
    });
    assert(gotError, "corrupt-file rename must emit the error chat message");
    assert(app.activeSession.id == meta.id, "corrupt-file rename must keep the active meta");
    gotList = false;
    receiveTimeout(dur!"seconds"(1), (immutable UiSessionList l) {
        gotList = true;
    });
    assert(gotList, "corrupt-file rename must still refresh the sidebar list");
}

@(
        "stale pending-delete clearing by the sidebar New handler and store-exception degradation in a sidebar handler")

unittest {
    import std.file : exists, rmdirRecurse, mkdirRecurse;
    import std.path : buildPath;

    // A store whose create() throws simulates a disk-full failure.
    static class ThrowingStore : SessionStore {
        this(string dir) {
            super(dir.Path);
        }

        override SessionMeta create() @trusted {
            throw new Exception("simulated disk full");
        }
    }

    auto tmpDir = buildPath("llmfun_test", "app_agent_new_throw");
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.sessionStore = new ThrowingStore(tmpDir);
    auto stale = SessionId("stale-pending");
    app.pendingDeleteId = stale;

    // The handler clears pendingDeleteId on entry even though the create() below throws; the exception is caught and logged as a chat message - the receive loop keeps running.
    app.doSidebarNew();
    assert(app.pendingDeleteId == SessionId.init,
            "New handler must clear stale pending delete on entry (A5)");
}

// sidebar snapshot keeps store order (updatedAt descending) with the active marker; clicking must not reorder
@("sidebar snapshot keeps store order")
unittest {
    import std.file : exists, mkdirRecurse, rmdirRecurse, write;
    import std.format : format;
    import std.path : buildPath;

    auto tmpDir = buildPath("llmfun_test", "app_agent_snapshot_order");
    // Clear any stale dir from a crashed earlier run so the deterministic store-order precondition cannot be disturbed by leftover files.
    if (exists(tmpDir))
        rmdirRecurse(tmpDir);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    // Three sessions with distinct updatedAt so the store order is deterministic: a (100) < c (200) < b (300) -> store order [b, c, a].
    auto a = SessionId("20260618-153045-a1b2");
    auto b = SessionId("20260618-153045-b3c4");
    auto c = SessionId("20260618-153045-c5d6");
    void writeSession(SessionId id, long updatedAt) {
        write(buildPath(tmpDir, id.get ~ ".json"),
                format(`{"title": "T", "createdAt": 100, "updatedAt": %d, "messages": []}`,
                    updatedAt));
    }

    writeSession(a, 100);
    writeSession(b, 300);
    writeSession(c, 200);

    auto store = new SessionStore(tmpDir.Path);
    assert(store.list().map!(s => s.id).array == [b, c, a],
            "store order must be updatedAt descending");

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.sessionStore = store;
    app.activeSession.id = c; // the middle one is active
    // Active UiMessenger pointed at this thread: sendSessionList() lands in this thread's own mailbox (same pattern as the rename-none test).
    app.uiMsg = new UiMessenger(thisTid, false);
    app.uiTid = thisTid;

    // Active session stays in store order, only marked active.
    app.sendSessionList();
    bool gotList = false;
    receiveTimeout(dur!"seconds"(1), (immutable UiSessionList l) {
        gotList = true;
        assert(l.items.length == 3);
        assert(l.items[0].id == b && !l.items[0].isActive,
            "most recent session leads regardless of the active marker");
        assert(l.items[1].id == c && l.items[1].isActive,
            "the active session must stay at its store position");
        assert(l.items[2].id == a && !l.items[2].isActive);
    });
    assert(gotList, "sendSessionList must emit the store-ordered snapshot");

    // Active at index 0 (store order): order unchanged, marked active.
    app.activeSession.id = b;
    app.sendSessionList();
    gotList = false;
    receiveTimeout(dur!"seconds"(1), (immutable UiSessionList l) {
        gotList = true;
        assert(l.items.length == 3);
        assert(l.items[0].id == b && l.items[0].isActive);
        assert(l.items[1].id == c && l.items[2].id == a, "snapshot must keep the store order");
    });
    assert(gotList, "sendSessionList must emit the store-ordered snapshot");

    // Active absent from the list: snapshot keeps the store order.
    app.activeSession.id = SessionId("20260618-153045-9999");
    app.sendSessionList();
    gotList = false;
    receiveTimeout(dur!"seconds"(1), (immutable UiSessionList l) {
        gotList = true;
        assert(l.items.length == 3);
        assert(l.items[0].id == b && l.items[1].id == c && l.items[2].id == a,
            "absent active id must leave the snapshot in store order");
        assert(!l.items[0].isActive && !l.items[1].isActive && !l.items[2].isActive);
    });
    assert(gotList, "sendSessionList must emit the store-ordered snapshot");

    // Empty store: empty snapshot, no throw.
    app.sessionStore = new SessionStore(buildPath(tmpDir, "empty_sub").Path);
    app.sendSessionList();
    gotList = false;
    receiveTimeout(dur!"seconds"(1), (immutable UiSessionList l) {
        gotList = true;
        assert(l.items.length == 0, "empty store must produce an empty snapshot");
    });
    assert(gotList, "sendSessionList must emit the empty snapshot");
}

// navigation never rewrites a session (updatedAt unchanged) and a dirty chat commits with an updatedAt bump
@("navigation never rewrites a session")
unittest {
    import my.filter : ReFilter;
    import std.file : exists, mkdirRecurse, rmdirRecurse, write;
    import std.format : format;
    import std.path : buildPath;

    auto tmpDir = buildPath("llmfun_test", "app_agent_switch_no_bump");
    if (exists(tmpDir))
        rmdirRecurse(tmpDir);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto cfg = testLlmConfig(tmpDir);
    auto store = new SessionStore(buildPath(tmpDir, "chat").Path);

    // Two sessions with distinct updatedAt: newer (300) and older (100).
    auto newer = SessionId("20260618-153045-b3c4");
    auto older = SessionId("20260618-153045-a1b2");
    void writeSession(SessionId id, long updatedAt) {
        write(buildPath(tmpDir, "chat", id.get ~ ".json"), format(
                `{"title": "T", "createdAt": 100, "updatedAt": %d,` ~ `"messages": [{"role": "user", "content": "hi"}]}`,
                updatedAt));
    }

    writeSession(newer, 300);
    writeSession(older, 100);

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.llmConf = cfg;
    app.sessionStore = store;
    app.agent_ = new Agent("main", cfg, null, null, null, ReFilter.init);
    auto newerFile = store.load(newer);
    assert(hasValue(newerFile), "setup: active session file must load");
    app.activeSession = orElse(newerFile, SessionFile()).meta;

    const long newerUpdatedBefore = app.activeSession.updatedAt;

    // Switching away from the clean active session must not rewrite it.
    app.switchToSession(older);
    auto afterSwitch = orElse(store.load(newer), SessionFile());
    assert(afterSwitch.meta.updatedAt == newerUpdatedBefore,
            "switching must not bump updatedAt of the session you leave");

    // Switching back: same guarantee for the other direction.
    const long olderUpdatedBefore = app.activeSession.updatedAt;
    app.switchToSession(newer);
    assert(orElse(store.load(older), SessionFile()).meta.updatedAt == olderUpdatedBefore,
            "switching back must not bump updatedAt either");
    assert(app.activeSession.id == newer, "setup: active is the newer session again");

    // A real content change (a user query) marks the chat dirty and the commit bumps updatedAt.
    const long beforeQuery = app.activeSession.updatedAt;
    app.agent_.addUserQuery("hello newer");
    app.chatDirty = true;
    app.commitActiveSession();
    assert(app.chatDirty == false, "a successful commit must clear the dirty flag");
    assert(app.activeSession.updatedAt >= beforeQuery, "a query commit must bump updatedAt");

    // A clean commit is a no-op: no further bump, no rewrite.
    const long afterQuery = app.activeSession.updatedAt;
    app.commitActiveSession();
    assert(app.activeSession.updatedAt == afterQuery, "a clean commit must not rewrite the file");
}

@("dispose() sweeps empty non-active sessions on exit")

unittest {
    import my.filter : ReFilter;
    import std.file : exists, mkdirRecurse, rmdirRecurse;
    import std.json : JSONValue;
    import std.path : buildPath;

    auto tmpDir = buildPath("llmfun_test", "app_agent_dispose_sweep");
    if (exists(tmpDir))
        rmdirRecurse(tmpDir);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto cfg = testLlmConfig(tmpDir);

    auto store = new SessionStore(buildPath(tmpDir, "chat").Path);
    auto active = store.create();
    auto emptyNonActive = store.create();
    auto nonEmpty = store.create();

    // nonEmpty gets one user message via save().
    JSONValue doc;
    doc["messages"] = JSONValue();
    doc["messages"].array = [];
    JSONValue userMsg;
    userMsg["role"] = "user";
    userMsg["content"] = "hello";
    doc["messages"].array ~= userMsg;
    assert(store.save(nonEmpty.id, nonEmpty, doc).userMessageCount == 1,
            "setup: non-empty session needs 1 user message");

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.llmConf = cfg;
    app.sessionStore = store;
    app.activeSession = active;
    app.agent_ = new Agent("main", cfg, null, null, null, ReFilter.init);
    // The in-memory chat carries the active session's user message, as in production after a query: the dirty flag is set so commitActiveSession() persists it BEFORE the sweep.
    app.agent_.addUserQuery("active hello");
    app.chatDirty = true;

    app.dispose(); // must not throw

    assert(!exists(buildPath(tmpDir, "chat", emptyNonActive.id.get ~ ".json")),
            "empty non-active session file must be swept on exit");
    assert(exists(buildPath(tmpDir, "chat", active.id.get ~ ".json")),
            "active session must survive dispose()");
    assert(exists(buildPath(tmpDir, "chat", nonEmpty.id.get ~ ".json")),
            "non-empty non-active session must survive");
    assert(orElse(store.load(active.id), SessionFile()).meta.userMessageCount == 1,
            "commit runs before the sweep: active session keeps its user message");
    assert(app.agent_ is null, "dispose() must clear the agent");
}

@("dispose() with a null session store")

unittest {
    import my.filter : ReFilter;
    import std.file : exists, mkdirRecurse, rmdirRecurse;
    import std.path : buildPath;

    auto tmpDir = buildPath("llmfun_test", "app_agent_dispose_null_store");
    if (exists(tmpDir))
        rmdirRecurse(tmpDir);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto cfg = testLlmConfig(tmpDir);

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.llmConf = cfg;
    // A failed setupSession leaves the store null while agent_ is set and the active id empty; dispose() must not dereference the store.
    app.agent_ = new Agent("main", cfg, null, null, null, ReFilter.init);
    app.dispose(); // must not throw
    assert(app.agent_ is null, "dispose() must clear the agent");
}

@("Test: dispose() keeps the active session even when empty")

unittest {
    import my.filter : ReFilter;
    import std.file : exists, mkdirRecurse, rmdirRecurse;
    import std.path : buildPath;

    auto tmpDir = buildPath("llmfun_test", "app_agent_dispose_all_empty");
    if (exists(tmpDir))
        rmdirRecurse(tmpDir);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto cfg = testLlmConfig(tmpDir);

    auto store = new SessionStore(buildPath(tmpDir, "chat").Path);
    auto active = store.create(); // stays empty on purpose
    auto emptyOther = store.create();

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.llmConf = cfg;
    app.sessionStore = store;
    app.activeSession = active;
    app.agent_ = new Agent("main", cfg, null, null, null, ReFilter.init);

    app.dispose(); // must not throw

    assert(exists(buildPath(tmpDir, "chat", active.id.get ~ ".json")),
            "active session must survive dispose() even when empty (W15)");
    assert(!exists(buildPath(tmpDir, "chat", emptyOther.id.get ~ ".json")),
            "empty non-active session must be swept on exit");
}

// switch-after-compression resets stat().startContext to the target chat's approxContextSize
//
// Regression lock for the activate pipeline: activateSession must call syncContextFromChat() AFTER chat.load so prevStat no longer carries the previous session's context. The "compressed" state is modeled headlessly (no server call): loading the small session and syncing leaves exactly the invariant a real compression leaves behind (prevStat.startContext == current chat context). A stale value would fail the asserts below.

unittest {
    import my.filter : ReFilter;
    import std.array : replicate;
    import std.file : exists, mkdirRecurse, rmdirRecurse;
    import std.json : JSONValue;
    import std.path : buildPath;

    auto tmpDir = buildPath("llmfun_test", "app_agent_switch_after_compression");
    if (exists(tmpDir))
        rmdirRecurse(tmpDir);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto cfg = testLlmConfig(tmpDir);

    auto store = new SessionStore(buildPath(tmpDir, "chat").Path);
    auto small = store.create(); // active session: small context
    auto large = store.create(); // switch target: clearly larger context

    // Small doc: one short user message (~2 tokens).
    JSONValue smallDoc;
    smallDoc["messages"] = JSONValue();
    smallDoc["messages"].array = [];
    JSONValue smallUser;
    smallUser["role"] = "user";
    smallUser["content"] = "short";
    smallDoc["messages"].array ~= smallUser;
    small = store.save(small.id, small, smallDoc);

    // Large doc: one long user message (400 chars -> ~200 tokens).
    JSONValue largeDoc;
    largeDoc["messages"] = JSONValue();
    largeDoc["messages"].array = [];
    JSONValue largeUser;
    largeUser["role"] = "user";
    largeUser["content"] = "x".replicate(400);
    largeDoc["messages"].array ~= largeUser;
    large = store.save(large.id, large, largeDoc);

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.llmConf = cfg;
    app.sessionStore = store;
    app.activeSession = small;
    app.agent_ = new Agent("main", cfg, null, null, null, ReFilter.init);

    // Model the post-compression state of the small session: the agent chat holds the (small) session history and prevStat is synced from it. staleStart is the value that would survive the switch if activateSession ever stopped calling syncContextFromChat.
    app.agent_.chat.load(smallDoc);
    app.agent_.syncContextFromChat();
    const long staleStart = app.agent_.stat().startContext;
    // Modeling precondition only: syncContextFromChat() sets prevStat from the chat by construction, so this cannot fail for a production bug - it just confirms the model matches what a real compression leaves.
    assert(staleStart == app.agent_.chat.approxContextSize,
            "setup: prevStat must hold the small session's context");

    app.switchToSession(large.id);

    assert(app.activeSession.id == large.id, "switch must land on the target session");
    // The regression lock: startContext is reset to the TARGET chat's size. A stale compressed value (staleStart) fails this assert.
    assert(app.agent_.stat().startContext == app.agent_.chat.approxContextSize,
            "stat().startContext must be reset to the target chat's context after switching");
    // Exact expectation derived from the payload defined above, immune to payload-size edits and to any plausible ApproxTokenSize change (both sides scale together), and still fails on a stale prevStat. The large payload is 400 'x' chars, the small one "short" (5 chars).
    import llm.common.config : ApproxTokenSize; // public, stable
    const long expectedLarge = largeUser["content"].str.length / ApproxTokenSize; // ~200
    assert(app.agent_.stat().startContext == expectedLarge,
            "stat().startContext must equal the target chat's exact approx context");
    assert(app.agent_.stat().startContext > staleStart,
            "target context must be clearly larger than the stale compressed value");
}

// delete-active falls back to the most recently updated remaining session and the snapshot drops the deleted id

unittest {
    import my.filter : ReFilter;
    import std.file : exists, mkdirRecurse, rmdirRecurse, write;
    import std.format : format;
    import std.path : buildPath;

    auto tmpDir = buildPath("llmfun_test", "app_agent_delete_active_fallback");
    if (exists(tmpDir))
        rmdirRecurse(tmpDir);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto cfg = testLlmConfig(tmpDir);
    auto store = new SessionStore(buildPath(tmpDir, "chat").Path);

    // Three sessions with distinct updatedAt: a (400, the active one to delete) > b (300, the expected fallback) > c (200).
    auto a = SessionId("20260618-153045-a1b2");
    auto b = SessionId("20260618-153045-b3c4");
    auto c = SessionId("20260618-153045-c5d6");
    // Test-only helper: userContent must be JSON-safe ASCII (no escaping). createdAt is arbitrary (100): only updatedAt matters for ordering.
    void writeSession(SessionId id, long updatedAt, string userContent) {
        write(buildPath(tmpDir, "chat", id.get ~ ".json"), format(
                `{"title": "T", "createdAt": 100, "updatedAt": %d,` ~ `"messages": [{"role": "user", "content": "%s"}]}`,
                updatedAt, userContent));
    }

    writeSession(a, 400, "hello-a");
    writeSession(b, 300, "hello-b");
    writeSession(c, 200, "hello-c");

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.llmConf = cfg;
    app.sessionStore = store;
    app.agent_ = new Agent("main", cfg, null, null, null, ReFilter.init);
    auto aFile = store.load(a);
    assert(hasValue(aFile), "setup: active session file must load");
    app.activeSession = orElse(aFile, SessionFile()).meta;
    // Active UiMessenger pointed at this thread: the fallback activation's sendSessionList() and the confirmation chat message land in this thread's own mailbox (same pattern as the rename-none test).
    app.uiMsg = new UiMessenger(thisTid, false);
    app.uiTid = thisTid;

    app.doDeleteSession(a);

    assert(app.activeSession.id == b,
            "delete-active must switch to the most recently updated remaining session");
    assert(!exists(buildPath(tmpDir, "chat", a.get ~ ".json")),
            "the deleted session file must be gone");
    assert(exists(buildPath(tmpDir, "chat", b.get ~ ".json")),
            "the fallback session file must survive");
    assert(exists(buildPath(tmpDir, "chat", c.get ~ ".json")),
            "the other remaining session file must survive");

    // Snapshot: deleted id absent; the fallback is the most recently updated remaining session, so it leads the store order and is marked active.
    bool gotList = false;
    receiveTimeout(dur!"seconds"(1), (immutable UiSessionList l) {
        gotList = true;
        assert(l.items.length == 2, "snapshot must exclude the deleted id");
        assert(l.items[0].id == b && l.items[0].isActive,
            "fallback session must lead the snapshot and be active");
        assert(l.items[1].id == c && !l.items[1].isActive);
    });
    assert(gotList, "delete-active must refresh the sidebar snapshot");

    // Confirmation chat message on the fallback path (the exact string locks the user-facing wording).
    bool gotDeleted = false;
    string lastChatMsg;
    receiveTimeout(dur!"seconds"(1), (UiChatMessage m) {
        gotDeleted = m.msg == "Session deleted: a1b2. Switched to 'T'.";
        lastChatMsg = m.msg; // keep for the diagnostic below
    });
    assert(gotDeleted, "delete-active must emit the switch confirmation, got: " ~ lastChatMsg);

    // Drain the remaining UI messages so this thread's mailbox stays clean for subsequent unittests (receiveTimeout scans past unmatched types, but other tests in this binary may match on them). The snapshot handler uses the immutable variant: UiSessionList is sent as cast(immutable), so a mutable handler would never match. INVARIANT: keep this handler list in sync with everything activateSession/sendSessionList/setStatusText can emit (a new UI message type added there would silently stay in the mailbox here).
    foreach (_; 0 .. 20) {
        bool drained = receiveTimeout(dur!"msecs"(50), (UiClearChat _) {}, (UiPipelineClear _) {
        }, (UiChatThinkMessage _) {}, (UiInitHistory _) {}, (UiStatusText _) {}, (UiChatMessage _) {
        }, (immutable UiSessionList _) {});
        if (!drained)
            break;
    }
}

// corrupt session files are skipped by listing and the sweep, and switchToSession keeps the current session on load failure

unittest {
    import my.filter : ReFilter;
    import std.file : exists, mkdirRecurse, rmdirRecurse, write;
    import std.path : buildPath;

    auto tmpDir = buildPath("llmfun_test", "app_agent_corrupt_files");
    if (exists(tmpDir))
        rmdirRecurse(tmpDir);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);

    auto cfg = testLlmConfig(tmpDir);
    auto store = new SessionStore(buildPath(tmpDir, "chat").Path);
    auto good = store.create();
    auto emptyOther = store.create();

    // Corrupt file with a valid id: present on disk, must never be listed, loaded, or swept.
    auto corrupt = SessionId("20260618-153046-0bad");
    auto corruptPath = buildPath(tmpDir, "chat", corrupt.get ~ ".json");
    write(corruptPath, "{ not json !!!");

    // Listing skips the corrupt file.
    auto listed = store.list();
    assert(listed.map!(s => s.id).canFind(good.id), "setup: good session listed");
    assert(!listed.map!(s => s.id).canFind(corrupt),
            "corrupt file must be absent from the store listing");

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.llmConf = cfg;
    app.sessionStore = store;
    app.agent_ = new Agent("main", cfg, null, null, null, ReFilter.init);
    auto goodFile = store.load(good.id);
    assert(hasValue(goodFile), "setup: good session file must load");
    app.activeSession = orElse(goodFile, SessionFile()).meta;
    app.uiMsg = new UiMessenger(thisTid, false);
    app.uiTid = thisTid;

    // Snapshot excludes the corrupt file. Both remaining sessions were created in the same second (updatedAt tie, id-desc tiebreak), so the exact order is not asserted here - only membership and the active marker (order semantics are covered by the snapshot-order test).
    app.sendSessionList();
    bool gotList = false;
    receiveTimeout(dur!"seconds"(1), (immutable UiSessionList l) {
        gotList = true;
        assert(l.items.length == 2, "snapshot must exclude the corrupt file");
        auto ids = l.items.map!(i => i.id).array;
        assert(ids.canFind(good.id) && ids.canFind(emptyOther.id),
            "snapshot must list both remaining sessions");
        foreach (item; l.items) {
            if (item.id == good.id) {
                assert(item.isActive, "the active session must be marked active");
            } else {
                assert(!item.isActive, "only the active session is marked");
            }
        }
    });
    assert(gotList, "sendSessionList must emit the snapshot");

    // switchToSession on the corrupt id: error message, current session kept (the exact string locks the user-facing wording).
    app.switchToSession(corrupt);
    bool gotError = false;
    receiveTimeout(dur!"seconds"(1), (UiChatMessage m) {
        gotError = m.msg
            == "error: Cannot load session '20260618-153046-0bad' (not found or corrupt). Staying in current session.";
    });
    assert(gotError, "switchToSession must emit the cannot-load error");
    assert(app.activeSession.id == good.id, "a failed load must keep the current session unchanged");

    // The sweep removes only list() candidates: the corrupt file is never a candidate and survives untouched, the empty non-active session is removed, and the kept (active) id is exempted. Note: good is empty from creation (store.create() writes messages: []); the failed switch commits nothing (the chat is clean), so the keep-exemption is what protects it here; non-empty survival is covered by the store-level tests.
    auto swept = store.sweepEmptySessions(good.id);
    assert(swept.length == 1 && swept[0] == emptyOther.id,
            "sweep must remove only the empty non-active session");
    assert(!exists(buildPath(tmpDir, "chat", emptyOther.id.get ~ ".json")),
            "the empty non-active session file must be swept");
    assert(exists(corruptPath), "the sweep must leave the corrupt file untouched");
    assert(exists(buildPath(tmpDir, "chat", good.id.get ~ ".json")),
            "the kept session file must survive the sweep");
}
