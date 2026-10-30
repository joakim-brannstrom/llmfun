/// Integration tests for AgentApp: slash dispatch, session persistence, and sidebar actions.
module llm.app_agent.tests;

import logger = std.logger;
import std.algorithm;
import std.array : empty, array, appender;
import std.conv : to, text;
import std.datetime : dur;
import std.exception : collectException;
import std.json : JSONType, JSONValue;
import std.string : startsWith, strip, join;
import std.sumtype : match;

import my.actor;
import my.path : Path, AbsolutePath;
import my.optional : Optional, hasValue, orElse;

import llm.agent;
import llm.agent_md;
import llm.app_agent.slash;
import llm.app_agent.ui;
import llm.app_agent;
import llm.app_config : UserConfig, userToLlmConfig, createRag;
import llm.chat;
import llm.config : RagConfig;
import llm.config;
import llm.metric.monitor : MetricMonitor;
import llm.query;
import llm.rag.dialogue_index : DialogueIndex;
import llm.rag.dialogue_worker : CompletionGate, DiDrained;
import llm.rag.rag : RAG;
import llm.rag.reasoning_index : ReasoningIndex, loadReasoningPrompt;
import llm.session : SessionId, SessionMeta, SessionFile, SessionStore, isValidId;
import llm.skill;
import llm.types : ServerStat, IStreamCallback, StreamMessage, StreamToolCall;
import llm.utility;
import llmfun_tui;

version (unittest) {
    import llm.common.config : EmbedConfig, RemoteEmbedConfig;
    import llm.common.embedder : Embedder, EmbedResult, EmbedError;
    import llm.test_util : testArea;

    // Minimal headless LlmConfig for constructing a real Agent in tests: one
    // code model (never contacted) plus a temp summary prompt file. dataDir is
    // intentionally NOT created so dispose()'s saveState() is a no-op - the
    // tests never write a state.json.
    //
    // Coupling invariants: the Agent is built with null
    // SkillManager/MetricMonitor/RAG (tolerated by the constructor) and a
    // model name that is never contacted; any future Agent constructor change
    // must keep this helper compiling and passing.
    package LlmConfig testLlmConfig(string tmpDir) {
        import std.file : mkdirRecurse, write;
        import std.path : buildPath;
        import llm.common.config : ServerConfig;

        auto promptDir = buildPath(tmpDir, "prompt");
        mkdirRecurse(promptDir);
        write(buildPath(promptDir, "SUMMARY.md"), "test summary prompt");
        writeDefaultNudgeFiles(promptDir);

        LlmConfig cfg;
        cfg.codeModels = [
            CodeModelConfig(server: ServerConfig.init, modelName: "test-model")
        ];
        cfg.activeCodeModelIndex = 0;
        cfg.promptDir = [promptDir.Path];
        cfg.dataDir = buildPath(tmpDir, "no-state").Path;
        return cfg;
    }

    /// Test seam for runAgent-level tests: `Agent.process` is public and
    /// virtual, so a subclass can feed the runToCompletion loop canned
    /// ProcessResults without an HTTP endpoint (same pattern as
    /// CannedProcessAgent in llm.agent.tests).
    private class CannedAgent : Agent {
        import llm.types : ProcessResult;
        import my.filter : ReFilter;

        ProcessResult[] canned;
        size_t served;

        this(string name, LlmConfig llmConf) {
            super(name, llmConf, null, null, null, ReFilter.init);
        }

        override ProcessResult process(bool delegate() interrupt) @trusted nothrow {
            return canned[served++];
        }
    }

    // Two-phase exit block fakes: the cheap-model seam for the real worker (module scope:
    // SummarizerFn is a plain function pointer and cannot capture; the factory mirrors the
    // worker test seam in llm.rag.dialogue_worker).
    private class ExitEmbedder : Embedder {
        override void destroy() {
        }

        override string modelName() {
            return "wke";
        }

        override long dimensions() {
            return 8;
        }

        override bool supportsTokenization() {
            return false;
        }

        override EmbedResult embedQuery(string text) {
            return embed(text);
        }

        override EmbedResult embedDocument(string text) {
            return embed(text);
        }

        EmbedResult embed(string text) {
            auto v = new float[8];
            foreach (ref f; v)
                f = 1.0f;
            return EmbedResult(v);
        }

        override EmbedResult embedQuery(int[] tokens) {
            return embed(tokens);
        }

        override EmbedResult embedDocument(int[] tokens) {
            return embed(tokens);
        }

        EmbedResult embed(int[] tokens) {
            return EmbedResult(EmbedError("no tokens"));
        }

        override int[] tokenize(string text, bool addSpecial) {
            return null;
        }

        override string detokenize(int[] tokens) {
            return null;
        }

        override int batchSize() {
            return 16;
        }
    }

    private Embedder exitEmbFactory(EmbedConfig config) {
        return new ExitEmbedder();
    }

    private string exitSummFake(string prompt, string traceText) {
        return "EXITBLOCK record body";
    }
}

@("registerBuiltinCommands registers the full built-in set with the expected arg modes")
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
@(
        "built-in dispatch: aliases terminate, unknown commands are safe, bare /plan and /code take the unknown path")
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

// TurnID header round-trip: the high-water mark written by
// `commitActiveSession` survives a `SessionStore.save` and seeds the counter
// on reload. Exercises the same chain commitActiveSession uses
// (`extra["next_turn_id"]` -> save -> load -> `Chat.load`) without a live
// Agent: `Chat` and `SessionStore` cover the whole seam.
@("TurnID header round-trip: high-water mark")
unittest {
    import std.datetime : Clock;
    import std.format : format;
    import std.json : JSONValue, JSONType;

    import my.optional : hasValue, orElse;
    import my.path;
    import llm.chat : Chat, turnIdOf;
    import llm.session : SessionStore, SessionFile;

    // stdTime (100ns resolution) keeps two runs in the same second from colliding on the temp dir.
    auto now = Clock.currTime();
    auto tmpDir = testArea("turnid_commit");
    ;
    scope (exit)
        tmpDir.cleanup();

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

// Regression: every agent turn that appends persisted content (tool traffic,
// the final taskDone answer) must be committed by processResult's per-step
// commit - a turn after the first commit of a run is otherwise silently
// dropped on session switch / app restart
@("every agent turn that appends persisted content must be committed")
unittest {
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

    auto tmpDir = testArea("app_agent_process_result_dirty");

    auto cfg = testLlmConfig(tmpDir);
    auto store = new SessionStore(buildPath(tmpDir, "chat").Path);

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.llmConf = cfg;
    app.sessionStore = store;
    // AgentApp's ctor leaves agent_ null (production creates it in
    // AppAgentActor.startSetup); tests that drive the Agent assign it
    // manually, as the sibling tests below.
    app.agent_ = new Agent("main", cfg, null, null, null, ReFilter.init);

    // The session as it looks after a committed query: one user message,
    // persisted directly through the store, then activated. Loading resets the
    // app's dirty flag (activateSession, before its replay loop), so the turn
    // below starts from a clean, persisted state.
    auto meta = store.create();
    JSONValue doc;
    JSONValue userMsg;
    userMsg["role"] = "user";
    userMsg["content"] = "what is 2+2?";
    doc["messages"] = JSONValue([userMsg]);
    app.activeSession = store.save(meta.id, meta, doc);
    app.switchToSession(meta.id);

    // Turn 2 - appends ONLY agent-layer content, exactly as production does:
    // handleToolCalls adds the taskDone ToolMessage (with the final answer in
    // save_data) and its ToolResponse (agent), process() reports the appended
    // slice through ProcessResult.chat (rval.chat = chat.lastResponses), and
    // runToCompletion hands it to processResult as the step hook (the
    // &this.processResult delegate). Without the per-turn commit this turn
    // is not persisted: the flag stays false and the commit is a no-op, so a
    // session switch/restart loses the final answer.
    JSONValue call;
    call["id"] = "call_1";
    call["type"] = "function";
    JSONValue fn;
    fn["name"] = "task_done";
    fn["arguments"] = "{}";
    call["function"] = fn;

    JSONValue sd;
    sd["taskDoneAnswer"] = JSONValue("The answer is 4.");

    app.agent_.chat.add(ToolMessage(null, JSONValue([call]), JSONValue.init, sd));
    app.agent_.chat.add(ToolResponse("4", "call_1", "task_done", true));

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

    // Replaying the persisted doc yields the final-answer message (the
    // replayed chat renders the green FinalAnswer header).
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

// Regression: the final-answer turn of a two-turn run. Turn 1 is a user query
// committed through the app-level submit primitives; turn 2 resets the flag
// and appends ONLY agent tool traffic (the taskDone ToolMessage carrying the
// final answer) - its persistence depends entirely on processResult's per-turn
// dirty-flag set, because the submit path never sets the flag again. That turn
// is otherwise silently dropped on session switch / app restart
@("final-answer turn after a committed user query is persisted")
unittest {
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

    auto tmpDir = testArea("app_agent_final_answer_persist");
    scope (exit)
        tmpDir.cleanup;

    auto cfg = testLlmConfig(tmpDir);
    auto store = new SessionStore(buildPath(tmpDir, "chat").Path);

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.llmConf = cfg;
    app.sessionStore = store;
    // AgentApp's ctor leaves agent_ null (production creates it in
    // AppAgentActor.startSetup); tests that drive the Agent assign it
    // manually, as the sibling tests below.
    app.agent_ = new Agent("main", cfg, null, null, null, ReFilter.init);

    // Establish the active session the way production does: create() writes
    // the file with messages: [], then switchToSession activates it (clean
    // loaded chat, flag false, activeSession set). Without this the turn
    // commits below would write under a default (empty) SessionId.
    auto meta = store.create();
    app.switchToSession(meta.id);

    // Turn 1: a user query committed through the app-level submit path -
    // addUserQuery + flag + commit, the primitives runAgent uses
    // (agent_.addUserQuery then chatDirty = true).
    app.agent_.addUserQuery("hello");
    app.chatDirty = true;
    app.commitActiveSession();

    // Turn 2: flag cleared (as after turn 1's commit), then the agent turn
    // appends ONLY agent-layer content, exactly as production does:
    // handleToolCalls adds the taskDone ToolMessage (with the final answer in
    // save_data) and its ToolResponse (agent), process() reports the appended
    // slice through ProcessResult.chat (rval.chat = chat.lastResponses), and
    // runToCompletion hands it to processResult as the step hook (the
    // &this.processResult delegate). (Turn 1 bypasses process(), so prevIndex
    // stays 0 and turn 2's lastResponses slice also replays the user message -
    // benign: printUser:false -> trace only; persistence serializes the full
    // history regardless.) Without the per-turn commit this turn is not
    // persisted: the flag stays false and the commit is a no-op, so a session
    // switch/restart lost the final answer. (The append + lastResponses
    // pattern is the production-exact seam; processResult only reads
    // result.chat.)
    app.chatDirty = false;

    JSONValue call;
    call["id"] = "call_1";
    call["type"] = "function";
    JSONValue fn;
    fn["name"] = "task_done";
    fn["arguments"] = "{}";
    call["function"] = fn;

    JSONValue sd;
    sd["taskDoneAnswer"] = JSONValue("The answer is 4.");

    app.agent_.chat.add(ToolMessage(null, JSONValue([call]), JSONValue.init, sd));
    app.agent_.chat.add(ToolResponse("4", "call_1", "task_done", true));

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

    auto tmpDir = testArea("app_agent_system_prompt_switch");
    scope (exit)
        tmpDir.cleanup;

    auto cfg = testLlmConfig(tmpDir);
    auto store = new SessionStore(buildPath(tmpDir, "chat").Path);

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.llmConf = cfg;
    app.sessionStore = store;
    // AgentApp's ctor leaves agent_ null (production creates it in AppAgentActor.startSetup); tests that drive the Agent assign it manually, as the sibling tests do.
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
    // The most safety-critical behavior of the dispatcher: a stale
    // `pendingDeleteId` must never make the next `/delete <n>` confirm-delete
    // without re-prompting. runAgent's parity guard
    // (`query.startsWith("/delete") && !slashCommands_.isRegistered(query)`)
    // clears /delete-prefixed NON-commands; the top rule clears everything
    // else; registered /delete-prefixed commands are left to their handlers.
    // The registry's unknown path does NOT clear pending state.
    import llm.app_config : UserConfig;

    // Blocked UiMessenger - the unknown path writes via writeln, never a null uiMsg dereference.
    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    auto stale = SessionId("stale-id");

    // Registered /delete-prefixed command: the dispatcher must NOT clear - the
    // handler owns the delete state machine. deleteprobe's handler does not
    // touch pendingDeleteId, so any dispatcher clear would be visible.
    app.registerSlashCommand(SlashCommand("deleteprobe", [], [],
            SlashArgMode.none, 900, (ref AgentApp a, string arg) => AgentStatus.active));
    app.pendingDeleteId = stale;
    assert(app.runAgent("/deleteprobe") == AgentStatus.active);
    assert(app.pendingDeleteId == stale,
            "dispatcher must not clear pending for registered /delete-prefixed commands");

    // `/deletefoo` (unregistered /delete-prefixed typo): the registry's
    // unknown path does not clear pending, so runAgent's guard is the ONLY
    // clearing path. Without it the next `/delete 3` would confirm-delete
    // immediately instead of re-prompting.
    app.pendingDeleteId = stale;
    assert(app.runAgent("/deletefoo") == AgentStatus.active);
    assert(app.pendingDeleteId == SessionId.init,
            "/delete-prefixed non-commands must clear a stale confirmation");

    // Non-/delete unknown: cleared by the top rule, not the registry.
    app.pendingDeleteId = stale;
    assert(app.runAgent("/nope") == AgentStatus.active);
    assert(app.pendingDeleteId == SessionId.init, "non-/delete inputs clear via the top rule");

    // `/delete` (registered, handler-owned): dispatch still reaches the
    // handler; the empty-arg error path clears pending.
    app.pendingDeleteId = stale;
    assert(app.runAgent("/delete") == AgentStatus.active);
    assert(app.pendingDeleteId == SessionId.init);
}

@("sidebar snapshot mapping (SessionMeta[] -> UiSessionItem[])")
unittest {
    SessionMeta a, b;
    a.id = SessionId("20260618-153045-a1b2");
    a.title = "Alpha";
    a.preview = "prev a";
    a.messageCount = 3;
    a.userMessageCount = 2;
    a.updatedAt = 1_781_000_000; // unix seconds
    b.id = SessionId("20260618-153045-b3c4");
    b.title = "Beta";
    b.preview = "prev b";
    b.messageCount = 5;
    b.userMessageCount = 1;
    b.updatedAt = 1_781_086_400;

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

    // v5 (task 24): updatedAt crosses as milliseconds, the one conversion, and
    // no row is running unless the active one is.
    assert(items[0].updatedAtMs == 1_781_000_000_000);
    assert(items[1].updatedAtMs == 1_781_086_400_000);
    assert(items[0].flags == 0 && items[1].flags == 0);

    // A zero timestamp stays zero: the rail reads it as "unknown" and draws a
    // flat list instead of a fabricated date.
    SessionMeta undated;
    undated.id = SessionId("20260618-153045-c5d6");
    undated.title = "Undated";
    assert(AgentApp.mapSessionItems([undated], SessionId.init)[0].updatedAtMs == 0);

    // Running is a property of the *active* row only, and only when the caller
    // says a run is in flight (the actor's busy state is the only source).
    auto running = AgentApp.mapSessionItems([a, b], b.id, true);
    assert(running[0].flags == 0);
    assert(running[1].flags == 1);
    auto idle = AgentApp.mapSessionItems([a, b], b.id, false);
    assert(idle[0].flags == 0 && idle[1].flags == 0);

    // Empty input -> empty snapshot; no active session -> no active row.
    assert(AgentApp.mapSessionItems([], SessionId.init).length == 0);
    auto noActive = AgentApp.mapSessionItems([a], SessionId.init);
    assert(noActive.length == 1 && !noActive[0].isActive);
    // ...and with no active session there is no row a run could belong to.
    assert(AgentApp.mapSessionItems([a], SessionId.init, true)[0].flags == 0);
}

@("sidebar invalid-id rejection for each action type")
unittest {
    import llm.app_config : UserConfig;

    // No store is configured: reaching the store would crash the test, so a green run proves rejection happens BEFORE any store access.
    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    auto bad = SessionId("bad-id");

    app.doSidebarSelect(bad);
    assert(app.pendingDeleteId == SessionId.init, "Select clears pending delete");

    app.doSidebarRename(bad, "x");
    assert(app.pendingDeleteId == SessionId.init, "Rename clears pending delete");

    app.doSidebarDelete(bad);
    assert(app.pendingDeleteId == SessionId.init, "Delete clears pending delete");
}

// sidebar rename input validation (empty title rejected, long non-empty title accepted - no length cap, matches /rename)
@("sidebar rename input validation")
unittest {
    import std.array : replicate;

    auto tmpDir = testArea("app_agent_rename_validate");
    scope (exit)
        tmpDir.cleanup;

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
    import std.file : write;
    import std.path : buildPath;

    auto tmpDir = testArea("app_agent_rename_none");
    scope (exit)
        tmpDir.cleanup;

    auto store = new SessionStore(tmpDir.Path);
    auto meta = store.create();
    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.sessionStore = store;
    app.activeSession = meta;
    // Recording sink: the handler's error chat message and the
    // sendSessionList() refresh are recorded synchronously, so the test can
    // observe "error emitted" and "list still refreshed" without a UI thread
    // or a mailbox.
    auto sink = new TuiRecordingSink();
    app.uiMsg = new UiMessenger(sink);

    // Unknown id (valid format, no file): rename -> none -> error message,
    // active meta unchanged, list still refreshed.
    auto unknown = SessionId("20260618-153045-ffff");
    app.doSidebarRename(unknown, "New title");
    assert(sink.lastChatMessage() == "error: Failed to rename session 'ffff'.",
            "rename-none must emit the error chat message");
    assert(app.activeSession.id == meta.id, "rename-none must keep the active meta");
    assert(app.activeSession.title == meta.title);
    assert(sink.lastSessionList().length == 1, "list still refreshed on rename-none");
    assert(sink.countOf("sessionList") == 1, "rename-none must still refresh the sidebar list");

    // Corrupt file (valid id, garbage JSON): same none path.
    write(buildPath(tmpDir, "20260618-153046-0bad.json"), "{ not json !!!");
    auto corrupt = SessionId("20260618-153046-0bad");
    app.doSidebarRename(corrupt, "New title");
    assert(sink.lastChatMessage() == "error: Failed to rename session '0bad'.",
            "corrupt-file rename must emit the error chat message");
    assert(app.activeSession.id == meta.id, "corrupt-file rename must keep the active meta");
    assert(sink.countOf("sessionList") == 2,
            "corrupt-file rename must still refresh the sidebar list (one refresh per rename call)");
}

@(
        "stale pending-delete clearing by the sidebar New handler and store-exception degradation in a sidebar handler")
unittest {
    // A store whose create() throws simulates a disk-full failure.
    static class ThrowingStore : SessionStore {
        this(string dir) {
            super(dir.Path);
        }

        override SessionMeta create() @trusted {
            throw new Exception("simulated disk full");
        }
    }

    auto tmpDir = testArea("app_agent_new_throw");
    scope (exit)
        tmpDir.cleanup;

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.sessionStore = new ThrowingStore(tmpDir);
    auto stale = SessionId("stale-pending");
    app.pendingDeleteId = stale;

    // The handler clears pendingDeleteId on entry even though the create()
    // below throws; the exception is caught and logged as a chat message - the
    // receive loop keeps running.
    app.doSidebarNew();
    assert(app.pendingDeleteId == SessionId.init,
            "New handler must clear stale pending delete on entry");
}

@(
        "sidebar snapshot keeps store order (updatedAt descending) with the active marker; clicking must not reorder")
unittest {
    import std.file : write;
    import std.path : buildPath;

    auto tmpDir = testArea("app_agent_snapshot_order");
    scope (exit)
        tmpDir.cleanup;

    // Three sessions with distinct updatedAt so the store order is
    // deterministic: a (100) < c (200) < b (300) -> store order [b, c, a].
    auto a = SessionId("20260618-153045-a1b2");
    auto b = SessionId("20260618-153045-b3c4");
    auto c = SessionId("20260618-153045-c5d6");
    void writeSession(SessionId id, long updatedAt) {
        write(buildPath(tmpDir, id.get ~ ".json"), i`{"title": "T", "createdAt": 100, "updatedAt": $(
                updatedAt), "messages": []}`.text);
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
    // Recording sink: each sendSessionList() snapshot is recorded
    // synchronously (same pattern as the rename-none test).
    auto sink = new TuiRecordingSink();
    app.uiMsg = new UiMessenger(sink);

    // Active session stays in store order, only marked active.
    app.sendSessionList();
    auto snap = sink.lastSessionList();
    assert(snap.length == 3);
    assert(snap[0].id == b && !snap[0].isActive,
            "most recent session leads regardless of the active marker");
    assert(snap[1].id == c && snap[1].isActive,
            "the active session must stay at its store position");
    assert(snap[2].id == a && !snap[2].isActive);
    assert(sink.countOf("sessionList") == 1, "sendSessionList must emit the store-ordered snapshot");

    // Active at index 0 (store order): order unchanged, marked active.
    app.activeSession.id = b;
    app.sendSessionList();
    snap = sink.lastSessionList();
    assert(snap.length == 3);
    assert(snap[0].id == b && snap[0].isActive);
    assert(snap[1].id == c && snap[2].id == a, "snapshot must keep the store order");
    assert(sink.countOf("sessionList") == 2, "sendSessionList must emit the store-ordered snapshot");

    // Active absent from the list: snapshot keeps the store order.
    app.activeSession.id = SessionId("20260618-153045-9999");
    app.sendSessionList();
    snap = sink.lastSessionList();
    assert(snap.length == 3);
    assert(snap[0].id == b && snap[1].id == c && snap[2].id == a,
            "absent active id must leave the snapshot in store order");
    assert(!snap[0].isActive && !snap[1].isActive && !snap[2].isActive);
    assert(sink.countOf("sessionList") == 3, "sendSessionList must emit the store-ordered snapshot");

    // Empty store: empty snapshot, no throw.
    app.sessionStore = new SessionStore(buildPath(tmpDir, "empty_sub").Path);
    app.sendSessionList();
    snap = sink.lastSessionList();
    assert(snap.length == 0, "empty store must produce an empty snapshot");
    assert(sink.countOf("sessionList") == 4, "sendSessionList must emit the empty snapshot");
}

@("the running flag is the actor's run state, so every snapshot in a run carries it (task 24)")
unittest {
    import std.file : write;
    import std.format : format;
    import std.path : buildPath;

    auto tmpDir = testArea("app_agent_running_flag");
    scope (exit)
        tmpDir.cleanup;

    auto a = SessionId("20260618-153045-a1b2");
    auto b = SessionId("20260618-153045-b3c4");
    write(buildPath(tmpDir, a.get ~ ".json"),
            `{"title": "A", "createdAt": 100, "updatedAt": 100, "messages": []}`);
    write(buildPath(tmpDir, b.get ~ ".json"),
            `{"title": "B", "createdAt": 100, "updatedAt": 300, "messages": []}`);

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.sessionStore = new SessionStore(tmpDir.Path);
    app.activeSession.id = a;
    auto sink = new TuiRecordingSink();
    app.uiMsg = new UiMessenger(sink);

    // No run: no row may be marked running, however the snapshot was reached.
    app.sendSessionList();
    assert(sink.lastSessionList().all!(item => item.flags == 0),
            "an idle actor marks no session running");

    // A run in flight: the *active* row carries bit 0 on every send. The flag lives on
    // the actor, so a rename or a switch served mid-run cannot report "no run" by
    // forgetting a parameter - that is the whole point of the member.
    app.runInFlight = true;
    app.sendSessionList();
    auto snap = sink.lastSessionList();
    size_t activeIndex = size_t.max;
    foreach (i, ref const item; snap) {
        if (item.id == a)
            activeIndex = i;
    }
    assert(activeIndex != size_t.max && snap[activeIndex].flags == 1,
            "the active session is marked running while the actor has a run in flight");
    foreach (i, ref const item; snap) {
        if (i != activeIndex)
            assert(item.flags == 0, "...and no other row claims the run");
    }

    // End of the run: the next snapshot clears it.
    app.runInFlight = false;
    app.sendSessionList();
    assert(sink.lastSessionList().all!(item => item.flags == 0),
            "the end-of-run snapshot clears the running flag");
}

@(
        "navigation never rewrites a session (updatedAt unchanged) and a dirty chat commits with an updatedAt bump")
unittest {
    import std.file : write;
    import std.path : buildPath;
    import my.filter : ReFilter;

    auto tmpDir = testArea("app_agent_switch_no_bump");
    scope (exit)
        tmpDir.cleanup;

    auto cfg = testLlmConfig(tmpDir);
    auto store = new SessionStore(buildPath(tmpDir, "chat").Path);

    // Two sessions with distinct updatedAt: newer (300) and older (100).
    auto newer = SessionId("20260618-153045-b3c4");
    auto older = SessionId("20260618-153045-a1b2");
    void writeSession(SessionId id, long updatedAt) {
        write(buildPath(tmpDir, "chat", id.get ~ ".json"), i`{"title": "T", "createdAt": 100, "updatedAt": $(
                updatedAt), "messages": [{"role": "user", "content": "hi"}]}`.text);
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

    // A real content change (a user query) marks the chat dirty and the commit
    // bumps updatedAt.
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
    import std.file : exists;
    import std.json : JSONValue;
    import std.path : buildPath;
    import my.filter : ReFilter;

    auto tmpDir = testArea("app_agent_dispose_sweep");
    scope (exit)
        tmpDir.cleanup;

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
    // The in-memory chat carries the active session's user message, as in
    // production after a query: the dirty flag is set so commitActiveSession()
    // persists it BEFORE the sweep.
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

    auto tmpDir = testArea("app_agent_dispose_null_store");
    scope (exit)
        tmpDir.cleanup;

    auto cfg = testLlmConfig(tmpDir);

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.llmConf = cfg;
    // A failed setupSession leaves the store null while agent_ is set and the
    // active id empty; dispose() must not dereference the store.
    app.agent_ = new Agent("main", cfg, null, null, null, ReFilter.init);
    app.dispose(); // must not throw
    assert(app.agent_ is null, "dispose() must clear the agent");
}

@("dispose() keeps the active session even when empty")
unittest {
    import std.file : exists;
    import std.path : buildPath;
    import my.filter : ReFilter;

    auto tmpDir = testArea("app_agent_dispose_all_empty");
    scope (exit)
        tmpDir.cleanup;

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
            "active session must survive dispose() even when empty");
    assert(!exists(buildPath(tmpDir, "chat", emptyOther.id.get ~ ".json")),
            "empty non-active session must be swept on exit");
}

//
// Regression lock for the activate pipeline: activateSession must call
// syncContextFromChat() AFTER chat.load so prevStat no longer carries the
// previous session's context. The "compressed" state is modeled headlessly (no
// server call): loading the small session and syncing leaves exactly the
// invariant a real compression leaves behind (prevStat.startContext == current
// chat context). A stale value would fail the asserts below.
@("switch-after-compression resets stat().startContext to the target chat's approxContextSize")
unittest {
    import my.filter : ReFilter;
    import std.array : replicate;
    import std.json : JSONValue;
    import std.path : buildPath;

    auto tmpDir = testArea("app_agent_switch_after_compression");
    scope (exit)
        tmpDir.cleanup;

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

    // Model the post-compression state of the small session: the agent chat
    // holds the (small) session history and prevStat is synced from it.
    // staleStart is the value that would survive the switch if activateSession
    // ever stopped calling syncContextFromChat.
    app.agent_.chat.load(smallDoc);
    app.agent_.syncContextFromChat();
    const long staleStart = app.agent_.stat().startContext;
    // Modeling precondition only: syncContextFromChat() sets prevStat from the
    // chat by construction, so this cannot fail for a production bug - it just
    // confirms the model matches what a real compression leaves.
    assert(staleStart == app.agent_.chat.approxContextSize,
            "setup: prevStat must hold the small session's context");

    app.switchToSession(large.id);

    assert(app.activeSession.id == large.id, "switch must land on the target session");
    // The regression lock: startContext is reset to the TARGET chat's size. A
    // stale compressed value (staleStart) fails this assert.
    assert(app.agent_.stat().startContext == app.agent_.chat.approxContextSize,
            "stat().startContext must be reset to the target chat's context after switching");
    // Exact expectation derived from the payload defined above, immune to
    // payload-size edits and to any plausible ApproxTokenSize change (both
    // sides scale together), and still fails on a stale prevStat. The large
    // payload is 400 'x' chars, the small one "short" (5 chars).
    import llm.common.config : ApproxTokenSize; // public, stable
    const long expectedLarge = largeUser["content"].str.length / ApproxTokenSize; // ~200
    assert(app.agent_.stat().startContext == expectedLarge,
            "stat().startContext must equal the target chat's exact approx context");
    assert(app.agent_.stat().startContext > staleStart,
            "target context must be clearly larger than the stale compressed value");
}

@(
        "delete-active falls back to the most recently updated remaining session and the snapshot drops the deleted id")
unittest {
    import std.file : write, exists;
    import std.path : buildPath;
    import my.filter : ReFilter;

    auto tmpDir = testArea("app_agent_delete_active_fallback");
    scope (exit)
        tmpDir.cleanup;

    auto cfg = testLlmConfig(tmpDir);
    auto store = new SessionStore(buildPath(tmpDir, "chat").Path);

    // Three sessions with distinct updatedAt: a (400, the active one to
    // delete) > b (300, the expected fallback) > c (200).
    auto a = SessionId("20260618-153045-a1b2");
    auto b = SessionId("20260618-153045-b3c4");
    auto c = SessionId("20260618-153045-c5d6");
    // Test-only helper: userContent must be JSON-safe ASCII (no escaping).
    // createdAt is arbitrary (100): only updatedAt matters for ordering.
    void writeSession(SessionId id, long updatedAt, string userContent) {
        write(buildPath(tmpDir, "chat", id.get ~ ".json"), i`{"title": "T", "createdAt": 100, "updatedAt": $(
                updatedAt), "messages": [{"role": "user", "content": "$(userContent)"}]}`.text);
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
    // Recording sink: the fallback activation's sendSessionList() and the
    // confirmation chat message are recorded synchronously (same pattern as
    // the rename-none test).
    auto sink = new TuiRecordingSink();
    app.uiMsg = new UiMessenger(sink);

    app.doDeleteSession(a);

    assert(app.activeSession.id == b,
            "delete-active must switch to the most recently updated remaining session");
    assert(!exists(buildPath(tmpDir, "chat", a.get ~ ".json")),
            "the deleted session file must be gone");
    assert(exists(buildPath(tmpDir, "chat", b.get ~ ".json")),
            "the fallback session file must survive");
    assert(exists(buildPath(tmpDir, "chat", c.get ~ ".json")),
            "the other remaining session file must survive");

    // Snapshot: deleted id absent; the fallback is the most recently updated
    // remaining session, so it leads the store order and is marked active.
    auto snap = sink.lastSessionList();
    assert(snap.length == 2, "snapshot must exclude the deleted id");
    assert(snap[0].id == b && snap[0].isActive,
            "fallback session must lead the snapshot and be active");
    assert(snap[1].id == c && !snap[1].isActive);
    assert(sink.countOf("sessionList") == 1, "delete-active must refresh the sidebar snapshot");

    // Confirmation chat message on the fallback path (the exact string locks
    // the user-facing wording). The replayed history went through
    // chatThinkMessage, so the confirmation is the only recorded chatMessage.
    assert(sink.chatMessages.length == 1 && sink.chatMessages[0] == "Session deleted: a1b2. Switched to 'T'.",
            "delete-active must emit the switch confirmation, got: " ~ sink.lastChatMessage());
}

@("corrupt session files are skipped by listing and the sweep, and switchToSession keeps the current session on load failure")
unittest {
    import std.file : write, exists;
    import std.path : buildPath;
    import my.filter : ReFilter;

    auto tmpDir = testArea("app_agent_corrupt_files");
    scope (exit)
        tmpDir.cleanup;

    auto cfg = testLlmConfig(tmpDir);
    auto store = new SessionStore(buildPath(tmpDir, "chat").Path);
    auto good = store.create();
    auto emptyOther = store.create();

    // Corrupt file with a valid id: present on disk, must never be listed,
    // loaded, or swept.
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
    // Recording sink: snapshots and chat messages are observed synchronously
    // (same pattern as the rename-none test).
    auto sink = new TuiRecordingSink();
    app.uiMsg = new UiMessenger(sink);

    // Snapshot excludes the corrupt file. Both remaining sessions were created
    // in the same second (updatedAt tie, id-desc tiebreak), so the exact order
    // is not asserted here - only membership and the active marker (ordering
    // is asserted by the store-order snapshot test).
    app.sendSessionList();
    auto snap = sink.lastSessionList();
    assert(snap.length == 2, "snapshot must exclude the corrupt file");
    auto ids = snap.map!(i => i.id).array;
    assert(ids.canFind(good.id) && ids.canFind(emptyOther.id),
            "snapshot must list both remaining sessions");
    foreach (item; snap) {
        if (item.id == good.id) {
            assert(item.isActive, "the active session must be marked active");
        } else {
            assert(!item.isActive, "only the active session is marked");
        }
    }
    assert(sink.countOf("sessionList") == 1, "sendSessionList must emit the snapshot");

    // switchToSession on the corrupt id: error message, current session kept
    // (the exact string locks the user-facing wording).
    app.switchToSession(corrupt);
    assert(sink.lastChatMessage() == "error: Cannot load session '20260618-153046-0bad' (not found or corrupt). Staying in current session.",
            "switchToSession must emit the cannot-load error");
    assert(app.activeSession.id == good.id, "a failed load must keep the current session unchanged");

    // The sweep removes only list() candidates: the corrupt file is never a
    // candidate and survives untouched, the empty non-active session is
    // removed, and the kept (active) id is exempted. Note: good is empty from
    // creation (store.create() writes messages: []); the failed switch commits
    // nothing (the chat is clean), so the keep-exemption is what protects it
    // here; non-empty survival is covered by the store-level tests.
    auto swept = store.sweepEmptySessions(good.id);
    assert(swept.length == 1 && swept[0] == emptyOther.id,
            "sweep must remove only the empty non-active session");
    assert(!exists(buildPath(tmpDir, "chat", emptyOther.id.get ~ ".json")),
            "the empty non-active session file must be swept");
    assert(exists(corruptPath), "the sweep must leave the corrupt file untouched");
    assert(exists(buildPath(tmpDir, "chat", good.id.get ~ ".json")),
            "the kept session file must survive the sweep");
}

// AppAgentActor integration
//
// Exercise the actor shell around AgentApp on a real my.actor System. The
// actor reports completion over a scoped supervisor actor's mailbox,
// pumped by the test thread with receiveTimeout (the pattern established
// in llm.rag and llm.mcp_server); every test creates the System with the
// no-arg makeSystem() and shuts it down in scope(exit) so no pool thread
// outlives the test.

/// Mirrors the onException body AppAgentActor uses when a handler throws:
/// report the failure to the supervisor, then exit with
/// unhandledException. The real hook is a private member of
/// AppAgentActor, so the test drives a shell with the same body.
private final class TestExceptionActor {
    private ActorRef self_;
    private WeakAddress out_;

    this(WeakAddress outAddr) {
        this.out_ = outAddr;
    }

    void onSpawn(ActorRef self) {
        self_ = self;
    }

    void boom() {
        throw new Exception("test exception");
    }

    void onException(Exception e) {
        logger.warning("TestExceptionActor exception: ", e.msg);
        dynSend(out_, "agentDone", AgentDone(1));
        sendExit(self_.address(), ExitReason.unhandledException);
    }
}

/// Watches `b`: when b shuts down, the DownMsg reports "down" to the
/// supervisor.
private final class TestWatcher {
    private ActorRef self_;
    private WeakAddress b;
    private WeakAddress out_;

    this(WeakAddress outAddr, WeakAddress b) {
        this.out_ = outAddr;
        this.b = b;
    }

    void onSpawn(ActorRef self) {
        self_ = self;
        monitor(self.address(), b);
        dynSend(out_, "watchEvent", "watching");
    }

    void onDownMessage(DownMsg d) {
        dynSend(out_, "downEvent", "down");
    }
}

/// A plain victim: die() exits with kill so watchers receive the DownMsg.
private final class TestVictim {
    private ActorRef self_;

    void onSpawn(ActorRef self) {
        self_ = self;
    }

    void die() {
        sendExit(self_.address(), ExitReason.kill);
    }
}

@("parseTuiBackendMode: value map, case-insensitivity, invalid fallback")
unittest {
    assert(parseTuiBackendMode("auto") == TuiBackendMode_Auto);
    assert(parseTuiBackendMode("gui") == TuiBackendMode_Gui);
    assert(parseTuiBackendMode("tui") == TuiBackendMode_Tui);
    assert(parseTuiBackendMode("TUI") == TuiBackendMode_Tui, "case-insensitive");
    assert(parseTuiBackendMode("") == TuiBackendMode_Auto, "empty -> Auto");
    assert(parseTuiBackendMode(null) == TuiBackendMode_Auto, "unset -> Auto");
    assert(parseTuiBackendMode("bogus") == TuiBackendMode_Auto, "invalid -> Auto (warns)");
}

@("resolveTuiBackendMode: CLI flag > env > Auto")
unittest {
    auto conf = UserConfig.AgentChatConfig.init;
    // The env value is injected ("" = unset) so the parallel runner's process
    // environment can never leak into the resolution.
    assert(resolveTuiBackendMode(conf, "") == TuiBackendMode_Auto, "no flag, no env -> Auto");
    assert(resolveTuiBackendMode(conf, "auto") == TuiBackendMode_Auto, "env auto -> Auto");
    assert(resolveTuiBackendMode(conf, "tui") == TuiBackendMode_Tui, "env tui -> Tui");
    assert(resolveTuiBackendMode(conf, "gui") == TuiBackendMode_Gui, "env gui -> Gui");
    assert(resolveTuiBackendMode(conf, "bogus") == TuiBackendMode_Auto, "invalid env -> Auto");
    // A CLI flag wins over every env value (empty, valid, invalid).
    conf.tui = true;
    foreach (v; ["", "auto", "tui", "gui", "bogus"])
        assert(resolveTuiBackendMode(conf, v) == TuiBackendMode_Tui,
                "--tui must win over env '" ~ v ~ "'");
    conf.gui = true; // both flags is a CLI usage error; gui wins if reached
    foreach (v; ["", "auto", "tui", "gui", "bogus"])
        assert(resolveTuiBackendMode(conf, v) == TuiBackendMode_Gui,
                "--gui must win over env '" ~ v ~ "'");
}

@("agent actor: uiTerminated sends AgentDone(0)")
unittest {
    auto sys = makeSystem();
    scope (exit)
        sys.shutdown();

    auto sup = scopedActor;

    auto conf = UserConfig.AgentChatConfig.init;
    auto a = sys.spawn!AppAgentActor(UserConfig.init, conf, &sys, sup.address());

    dynSend(a, "uiTerminated");
    int code = -1;
    bool got = sup.receiveTimeout(5.dur!"seconds", (AgentDone ad) {
        code = ad.code;
    });
    assert(got && code == 0, "expected AgentDone(0) within 5s");
}

@("agent actor: diDegraded leaves the actor alive")
unittest {
    auto sys = makeSystem();
    scope (exit)
        sys.shutdown();

    auto sup = scopedActor;

    auto conf = UserConfig.AgentChatConfig.init;
    auto a = sys.spawn!AppAgentActor(UserConfig.init, conf, &sys, sup.address());

    // The degraded hook only logs; a follow-up message must still be
    // answered (actor alive).
    dynSend(a, "diDegraded", "no embedder available");
    dynSend(a, "uiTerminated");

    int code = -1;
    bool got = sup.receiveTimeout(5.dur!"seconds", (AgentDone ad) {
        code = ad.code;
    });
    assert(got && code == 0, "expected AgentDone(0) within 5s");
}

@("agent actor: onException sends AgentDone(1)")
unittest {
    auto sys = makeSystem();
    scope (exit)
        sys.shutdown();

    auto sup = scopedActor;

    auto a = sys.spawn!TestExceptionActor(sup.address());
    dynSend(a, "boom");

    int code = -1;
    bool got = sup.receiveTimeout(5.dur!"seconds", (AgentDone ad) {
        code = ad.code;
    });
    assert(got && code == 1, "expected AgentDone(1) within 5s");
}

@("agent actor: monitor receives DownMsg on kill")
unittest {
    auto sys = makeSystem();
    scope (exit)
        sys.shutdown();

    auto sup = scopedActor;

    auto b = sys.spawn!TestVictim();
    auto a = sys.spawn!TestWatcher(sup.address(), b.weakRef);

    // "watching" proves the MonitorRequest was queued on b before "die".
    string marker = "";
    bool got = sup.receiveTimeout(5.dur!"seconds", (string s) { marker = s; });
    assert(got && marker == "watching", "watcher must monitor before B dies");

    dynSend(b, "die");
    marker = "";
    got = sup.receiveTimeout(5.dur!"seconds", (string s) { marker = s; });
    assert(got && marker == "down", "watcher must receive DownMsg within 5s");
}

@("agent wiring: actor DI ctor spawns a live worker that answers a drain")
unittest {
    import llm.common.config : ServerConfig, EmbedConfig, RemoteEmbedConfig;
    import llm.test_util : TestArea, testArea;

    // Runtime verification of the production wiring: the actor
    // DialogueIndex ctor with a NULL
    // embedder factory - production shape: the worker resolves from the
    // process-global registry, empty in tests, so the worker degrades
    // and its one-shot DiDegraded lands on sup, where it is stashed -
    // must leave a LIVE worker handle, and the worker must answer a
    // drain. The RiJob-receipt half is covered by the reasoning_index
    // E2E test (same ctor, capture actor).
    auto tmpDir = testArea("agent_wiring_worker", __FILE__, __LINE__);
    scope (exit)
        tmpDir.cleanup();

    auto sup = scopedActor;
    auto sys = makeSystem();
    scope (exit)
        sys.shutdown();

    auto cfg = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wiring-embed", dimensions: 8));
    auto ragCfg = RagConfig(windowOverlapPercent: 10);
    auto di = new DialogueIndex(tmpDir, cfg, ragCfg, &sys, null,
            SummaryModelConfig.init, "", null, sup.address());
    scope (exit)
        di.dispose();

    // (a) the handle is live right after the ctor...
    assert(!di.worker.weakRef.empty, "worker handle must be live after the actor ctor");
    // (b) ...and the worker answers a drain (spawn + mailbox proven).
    auto gate = new CompletionGate;
    di.beginDispose(sup.address(), gate);
    assert(sup.receiveTimeout(30.dur!"seconds", (DiDrained _) {}), "worker must answer the drain");
    gate.close();
}

// Two-phase exit integration: a test AgentApp with the REAL worker (the production DialogueIndex
// actor ctor) and the cheap-model seam (fake embedder + summarizer). The exit is driven through
// the same methods the actor's funnel calls: beginDispose (what startExit does for
// uiTerminated) must answer DiDrained within ~10 s from the real worker BEFORE finishDispose
// (what finishExit does) truncates the session store. The full startSetup is deliberately not
// driven: it also spawns the interactive TUI actor (terminal I/O), which is out of
// unittest scope: the test covers the exit sequence, not full startup realism.
// The AgentDone half runs the REAL
// actor funnel (startExit -> finishExit -> agentDone) on its default no-drain app: the actor's
// private app field cannot be wired without startSetup, so the funnel's
// two halves (beginDispose/finishDispose on a live AgentApp) are proven on the real worker above, and the
// funnel's reply/cleanup/AgentDone path here.
@("agent exit: the real worker drains before the store is truncated")
unittest {
    import core.thread : Thread;
    import std.datetime : Clock;
    import std.file : exists, mkdirRecurse;
    import std.path : buildPath;
    import llm.rag.database : Database, openDatabase;
    import llm.rag.dialogue_worker : DiEpisode, DiJob;
    import llm.test_util : TestArea, testArea;
    import llm.common.config : EmbedConfig, RemoteEmbedConfig, ServerConfig;
    import my.optional : None;

    auto tmpDir = testArea("agent_exit_two_phase", __FILE__, __LINE__);
    scope (exit)
        tmpDir.cleanup();

    // Prompt fixture (SUMMARY.md + default nudge templates) and a real Agent config; point the
    // store, the worker dir and the state file at the test area.
    auto llmConf = testLlmConfig(tmpDir);
    llmConf.dataDir = buildPath(tmpDir, "data").Path;
    mkdirRecurse(llmConf.dataDir.toString); // saveState must actually write state.json
    llmConf.chatDir = buildPath(tmpDir, "chat").Path;
    llmConf.dialogueDir = buildPath(tmpDir, "dialogue").Path;
    llmConf.embedConfig = EmbedConfig(RemoteEmbedConfig(server: ServerConfig(url: "http://127.0.0.1:0"),
            modelName: "wke", dimensions: 8));

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.llmConf = llmConf;
    app.sessionStore = new SessionStore(llmConf.chatDir.AbsolutePath);
    app.activeSession = app.sessionStore.create();
    auto swept = app.sessionStore.create(); // empty, non-active: the exit sweep target

    auto sys = makeSystem();
    scope (exit)
        sys.shutdown();
    auto sup = scopedActor;

    // The real worker (actor path): the production ctor spawns it on sys; the fake embedder
    // factory + summarizer are the cheap-model seam (no network, no LLM spend).
    auto ragCfg = RagConfig(windowOverlapPercent: 10, nBatch: 1, maxChunksPerTopic: 512);
    app.dialogueIndex = new DialogueIndex(llmConf.dialogueDir.AbsolutePath, llmConf.embedConfig,
            ragCfg, &sys, &exitEmbFactory, llmConf.summaryModel, "",
            &exitSummFake, sup.address());

    // A real Agent: finishDispose's commit/sweep/saveState block runs only when agent_ is set.
    app.agent_ = new Agent("main", llmConf, null, null);

    // One episode for the worker to index before the exit; bounded wait for it to commit.
    string sid = "20240101-120000-ab23";
    dynSend(app.dialogueIndex.worker, "diJob", DiJob(sid,
            [
                DiEpisode("d_20240101_120000_ab23__t1_1__1000".idup,
                "EXITBLOCK episode body".idup, 1)
    ]));
    auto dbPath = buildPath(llmConf.dialogueDir.toString, sid ~ ".db");
    auto deadline = Clock.currTime + 5.dur!"seconds";
    bool recorded = false;
    while (!recorded && Clock.currTime < deadline) {
        if (exists(dbPath)) {
            auto dbOpt = openDatabase(dbPath.AbsolutePath, "wke", 8, readOnly: true);
            if (dbOpt.hasValue) {
                auto db = dbOpt.match!((Database d) => d, (None _) => Database.init);
                scope (exit)
                    db.destroy;
                recorded = db.queryTextSearch("EXITBLOCK", 10).length >= 1;
            }
        }
        if (!recorded)
            Thread.sleep(5.dur!"msecs"); // allowlisted unittest backoff: bounded completion poll
    }
    assert(recorded, "the episode must be indexed before the drain under test");

    // startExit half: enqueue the worker's drain and wait for the reply.
    auto gate = new CompletionGate;
    auto drainStart = Clock.currTime;
    assert(app.beginDispose(sup.address(), gate), "beginDispose must enqueue the drain");
    assert(sup.receiveTimeout(10.dur!"seconds", (DiDrained _) {}),
            "the real worker must answer the drain within ~10 s");
    gate.close();
    assert(Clock.currTime - drainStart < 10.dur!"seconds",
            "the drain must complete within ~10 s, before the cleanup phase");

    // finishExit half (after DiDrained): cleanup runs now that the drain is done.
    app.finishDispose();

    // Store truncation: the empty non-active session is swept, the active one is exempt, the
    // state file is written, and the drained resources are cleared.
    assert(!exists(buildPath(llmConf.chatDir.toString, swept.id.get ~ ".json")),
            "the empty non-active session must be swept on exit");
    assert(exists(buildPath(llmConf.chatDir.toString,
            app.activeSession.id.get ~ ".json")), "the active session is exempt from the sweep");
    assert(exists(buildPath(llmConf.dataDir.toString, "state.json")),
            "saveState must run at finishDispose");
    assert(app.dialogueIndex is null, "dialogueIndex must be cleared");
    assert(app.agent_ is null, "agent_ must be cleared");

    // The AgentDone code: the real actor funnel (startExit -> finishExit -> agentDone).
    auto a = sys.spawn!AppAgentActor(UserConfig.init,
            UserConfig.AgentChatConfig.init, &sys, sup.address());
    dynSend(a, "uiTerminated");
    int code = -1;
    bool got = sup.receiveTimeout(10.dur!"seconds", (AgentDone ad) {
        code = ad.code;
    });
    assert(got && code == 0, "expected AgentDone(0) from the actor funnel within ~10 s");
}

// NR derivation: the single shared displayable predicate drives both the
// ordinal walk and the display decision - live batches are numbered by
// offset + position over the contiguous history tail, /clear restarts the
// count at [1], and a session switch replays the other session's own NRs
// (a measured turn shows the same [NR] and [TIME] as live).
@("NR derivation: predicate, live offset, clear restart, session switch")
unittest {
    import std.path : buildPath;
    import std.json : JSONValue;
    import std.datetime : Duration, dur;
    import llm.agent : Agent;
    import llm.chat;
    import llm.session : SessionStore;
    import llm.types : ProcessResult;
    import my.filter : ReFilter;

    auto tmpDir = testArea("app_agent_nr");
    scope (exit)
        tmpDir.cleanup;

    auto cfg = testLlmConfig(tmpDir);
    auto store = new SessionStore(buildPath(tmpDir, "chat").Path);

    // 1. Predicate: the four arms, plus the two non-displayable Message cases
    assert(isDisplayable(Chat.MessageT(Message(Role.assistant, false, "a", null)), cfg));
    assert(isDisplayable(Chat.MessageT(Message(Role.user, true, "query", null)), cfg));
    assert(!isDisplayable(Chat.MessageT(Message(Role.user, false, "nudge", null)), cfg));
    assert(!isDisplayable(Chat.MessageT(Message(Role.system, false, "sys", null)), cfg));
    JSONValue call;
    call["id"] = "c1";
    call["type"] = "function";
    JSONValue fn;
    fn["name"] = "query_semantic";
    fn["arguments"] = "{}";
    call["function"] = fn;
    assert(isDisplayable(Chat.MessageT(ToolMessage(null, JSONValue([call]))), cfg));
    assert(isDisplayable(Chat.MessageT(ToolResponse("ok", "c1", "query_semantic", true)), cfg));
    assert(!isDisplayable(Chat.MessageT(ToolResponse("done", "c2", "task_done", true)), cfg));
    assert(!isDisplayable(Chat.MessageT(ToolResponse("out", "c3", "pipeline_output", true)), cfg));
    assert(isDisplayable(Chat.MessageT(VisionMessage("see", "data:image/png;base64,x")), cfg));
    assert(displayableCount([
        Chat.MessageT(Message(Role.system, false, "s", null)),
        Chat.MessageT(Message(Role.user, false, "n", null)),
        Chat.MessageT(Message(Role.user, true, "q", null)),
        Chat.MessageT(Message(Role.assistant, false, "a", null))
    ], cfg) == 2);

    // 2. Live batch: offset + position; hidden result and nudge consume none
    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.llmConf = cfg;
    app.sessionStore = store;
    app.agent_ = new Agent("main", cfg, null, null, null, ReFilter.init);
    auto sink = new TuiRecordingSink();
    app.uiMsg = new UiMessenger(sink);
    app.activeSession = store.create();

    app.agent_.chat.setSystemPrompt("sys");
    app.agent_.chat.add(Message(Role.assistant, false, "a1", null));
    app.agent_.chat.add(Message(Role.assistant, false, "a2", null)); // offset = 2

    auto q = Message(Role.user, true, "hello", null); // echoed before addUserQuery
    auto a3 = Message(Role.assistant, false, "hello back", null);
    a3.setProcessTime(MessageTime(432.dur!"msecs"));
    auto tm = ToolMessage("thinking", JSONValue([call]));
    tm.setProcessTime(MessageTime(120.dur!"msecs"));
    auto tr = ToolResponse("ok", "c1", "query_semantic", true);
    tr.setProcessTime(MessageTime(42.dur!"msecs"));
    auto trh = ToolResponse("done", "c2", "task_done", true); // hidden
    auto nudge = Message(Role.user, false, "keep going", null);

    // Mirror the production flow: the agent appends the turn's messages to
    // the chat BEFORE the step hook (processResult) sees them, so the batch
    // is the contiguous tail of the history.
    app.agent_.chat.add(q);
    app.agent_.chat.add(a3);
    app.agent_.chat.add(tm);
    app.agent_.chat.add(tr);
    app.agent_.chat.add(trh);
    app.agent_.chat.add(nudge);

    ProcessResult result;
    result.chat = [
        Chat.MessageT(q), Chat.MessageT(a3), Chat.MessageT(tm),
        Chat.MessageT(tr), Chat.MessageT(trh), Chat.MessageT(nudge)
    ];
    app.processResult(result);

    assert(sink.stamps.length == 3, "user query, hidden result and nudge are not displayed live");
    assert(sink.calls[0].kind == "chatThinkMessage"
            && sink.calls[0].payload.startsWith("assistant: hello back"));
    assert(sink.stamps[0].nr == 4); // 2 (offset) + 2nd displayable of the batch (q consumed 3)
    assert(sink.stamps[0].time.match!((Duration d) { return d == 432.dur!"msecs"; },
            (NoProcessingTime) { return false; }));
    assert(sink.stamps[1].nr == 5);
    assert(sink.stamps[2].nr == 6);
    assert(sink.stamps[2].time.match!((Duration d) { return d == 42.dur!"msecs"; },
            (NoProcessingTime) { return false; }));

    // 3. taskDone finalAnswer carries the same (nr, time) as its tool-call line
    sink.chatMessages.length = 0;
    sink.stamps.length = 0;
    JSONValue sd;
    sd["taskDoneAnswer"] = JSONValue("The answer is 4.");
    auto tm2 = ToolMessage(null, JSONValue([call]), JSONValue.init, sd);
    tm2.setProcessTime(MessageTime(555.dur!"msecs"));
    app.agent_.chat.add(tm2);
    app.agent_.chat.add(trh);
    ProcessResult r2;
    r2.chat = [Chat.MessageT(tm2), Chat.MessageT(trh)];
    app.processResult(r2);
    assert(sink.chatMessages.length == 0); // tool-call line is a think message; hidden result sends nothing
    assert(sink.stamps.length == 2, "tool-call line and finalAnswer are both recorded");
    assert(sink.stamps[0].nr == 7 && sink.stamps[1].nr == 7); // same ordinal
    assert(sink.stamps[1].time == sink.stamps[0].time);

    // 4. /clear: history truncated to the system prompt -> next is [1]
    app.agent_.chat.clear;
    ProcessResult r3;
    r3.chat = [Chat.MessageT(Message(Role.assistant, false, "fresh", null))];
    app.processResult(r3);
    assert(sink.stamps[2].nr == 1);

    // 5. Session switch: the other session's own NRs from [1], and a replay
    //    of a measured turn shows the same [NR] and [TIME] as live
    JSONValue doc;
    JSONValue m1;
    m1["role"] = "user";
    m1["content"] = "what is 2+2?";
    m1["save_data"] = JSONValue(["user": JSONValue(true)]);
    JSONValue m2;
    m2["role"] = "assistant";
    m2["content"] = "4";
    JSONValue sd2;
    sd2["processMs"] = JSONValue(1200);
    m2["save_data"] = sd2;
    doc["messages"] = JSONValue([m1, m2]);
    auto metaB = store.create();
    store.save(metaB.id, metaB, doc); // sibling-test order: persist before activating
    app.switchToSession(metaB.id); // commit A, then load + replay B from scratch
    assert(sink.stamps[sink.stamps.length - 2].nr == 1); // "what is 2+2?" replayed as [1]
    assert(sink.stamps[sink.stamps.length - 1].nr == 2);
    assert(sink.stamps[sink.stamps.length - 1].time.match!((Duration d) {
            return d == 1200.dur!"msecs";
        }, (NoProcessingTime) { return false; }));
}

// The user-query echo is sent from runAgent AFTER the turn-start doCompress.
// Its NR is the displayable count of the possibly-compressed history - the
// query's true ordinal, no +1, and a compression that purged displayable
// prefix messages cannot misnumber it (the count is read after doCompress
// returns, so any rewrite is reflected).
// The isActive() guard keeps the one-shot CLI output byte-stable (the
// startSetup one-shot branch runs with the default TuiBlockedSink, which is
// inactive and has no chat line for the prompt today).
@("runAgent: user-query echo NR is the query's true ordinal")
unittest {
    import std.path : buildPath;
    import std.datetime : Duration, dur;
    import llm.chat;
    import llm.session : SessionStore;
    import llm.types : ProcessResult;

    auto tmpDir = testArea("app_agent_echo");
    scope (exit)
        tmpDir.cleanup;

    auto cfg = testLlmConfig(tmpDir);
    auto store = new SessionStore(buildPath(tmpDir, "chat").Path);

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.llmConf = cfg;
    app.sessionStore = store;
    auto agent = new CannedAgent("main", cfg);
    app.agent_ = agent;
    auto sink = new TuiRecordingSink();
    app.uiMsg = new UiMessenger(sink);
    app.activeSession = store.create();
    clearStopAgent();

    // Non-displayable: consumes no ordinal. Set first: setSystemPrompt
    // replaces history[0] (it would clobber the first seeded message).
    app.agent_.chat.setSystemPrompt("sys");
    // Two prior displayable messages: the query lands at ordinal 3.
    app.agent_.chat.add(Message(Role.assistant, false, "a1", null));
    app.agent_.chat.add(Message(Role.assistant, false, "a2", null));

    agent.canned = [ProcessResult(status: ProcessResult.Status.networkFailure)];
    app.runAgent("hello");

    // The echo is the first persistent send, numbered 3 - the query's true
    // ordinal in the persisted history (displayable count AFTER addUserQuery
    // and the no-op turn-start doCompress), not a pre-derived 2 + 1.
    assert(sink.stamps.length == 1, "only the echo is persistent for an empty batch");
    assert(sink.calls[0].kind == "chatMessage" && sink.calls[0].payload == "hello");
    assert(sink.stamps[0].nr == 3);
    assert(sink.stamps[0].time.match!((Duration d) { return false; }, (NoProcessingTime) {
            return true;
        }));
    auto msgs = app.agent_.chat.getMessages();
    assert(msgs.length == 4); // system + a1 + a2 + query
    assert(msgs[3].match!((Message m) {
            return m.isUserQuery && m.content == "hello";
        }, (_) => false), "the query is the last persisted entry");
    assert(displayableCount(msgs, cfg) == 3, "the echo NR equals the persisted ordinal");

    // Slash-consumed query: no persistent echo (query is consumed before the
    // loop; the unknown-command notice is a display-only nr=0 line).
    auto stampCount = sink.stamps.length;
    app.runAgent("/nope");
    assert(sink.stamps.length == stampCount + 1, "the unknown-command notice is sent");
    assert(sink.stamps[$ - 1].nr == 0, "a consumed slash query must not be echoed");
    assert(sink.chatMessages[$ - 1].startsWith("system: Unknown command"));

    // One-shot byte-stability premise: the default sink is inactive, so the
    // startSetup one-shot branch never emits the echo line.
    auto oneShot = AgentApp(UserConfig.AgentChatConfig.init);
    assert(!oneShot.uiMsg.isActive(), "the default one-shot sink is inactive");
}

// Persistence gate: processMs through the REAL persistence path -
// commit -> file -> load -> replay. The committed session file carries
// processMs for every measured message (system stripped at commit), and a
// session switch replays the same [NR] (the ordinal in the history - never
// renumbered) and the original [TIME] for measured messages; unmeasured
// messages replay as NoProcessingTime (no suffix downstream).
@("replay after session switch: same [NR] and original [TIME] as live")
unittest {
    import std.file : readText;
    import std.path : buildPath;
    import std.json : JSONValue, JSONType, parseJSON;
    import std.datetime : Duration, dur;
    import llm.agent : Agent;
    import llm.chat;
    import llm.session : SessionStore;
    import llm.types : ProcessResult;
    import my.filter : ReFilter;

    auto tmpDir = testArea("app_agent_nr_replay");
    scope (exit)
        tmpDir.cleanup;

    auto cfg = testLlmConfig(tmpDir);
    auto store = new SessionStore(buildPath(tmpDir, "chat").Path);
    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.llmConf = cfg;
    app.sessionStore = store;
    app.agent_ = new Agent("main", cfg, null, null, null, ReFilter.init);
    auto sink = new TuiRecordingSink();
    app.uiMsg = new UiMessenger(sink);

    // Session A (active) with two pre-existing displayable messages;
    // session B exists for the switch away (create() already wrote its
    // file with messages: []).
    auto metaA = store.create();
    auto metaB = store.create();
    app.activeSession = metaA;
    app.agent_.chat.setSystemPrompt("sys");
    app.agent_.chat.add(Message(Role.assistant, false, "a1", null));
    app.agent_.chat.add(Message(Role.assistant, false, "a2", null));
    // Direct chat.add is test-only: the production submit path sets the
    // dirty flag itself, so set it explicitly for the pre-turn commit.
    app.chatDirty = true;
    app.commitActiveSession();

    // Live turn: user query + measured reply/tool traffic. Mirror the
    // production flow: the agent appends the turn's messages before the
    // step hook, so the batch is the contiguous tail of the history.
    auto q = Message(Role.user, true, "hello", null);
    auto a3 = Message(Role.assistant, false, "hello back", null);
    a3.setProcessTime(MessageTime(432.dur!"msecs"));
    JSONValue call;
    call["id"] = "c1";
    call["type"] = "function";
    JSONValue fn;
    fn["name"] = "query_semantic";
    fn["arguments"] = "{}";
    call["function"] = fn;
    auto tm = ToolMessage("thinking", JSONValue([call]));
    tm.setProcessTime(MessageTime(120.dur!"msecs"));
    auto tr = ToolResponse("ok", "c1", "query_semantic", true);
    tr.setProcessTime(MessageTime(42.dur!"msecs"));
    app.agent_.chat.add(q);
    app.agent_.chat.add(a3);
    app.agent_.chat.add(tm);
    app.agent_.chat.add(tr);
    ProcessResult result;
    result.chat = [
        Chat.MessageT(q), Chat.MessageT(a3), Chat.MessageT(tm), Chat.MessageT(tr)
    ];
    app.processResult(result); // commits session A at the end

    // processMs actually persisted in the session file (system stripped at
    // commit: a1, a2, q, a3, tm, tr)
    auto fileJson = parseJSON(readText(buildPath(tmpDir, "chat", metaA.id.get ~ ".json")));
    auto fileMsgs = fileJson["messages"].array;
    assert(fileMsgs.length == 6, "system is stripped at commit; the file holds a1,a2,q,a3,tm,tr");
    bool hasProcessMs(JSONValue m) {
        return ("save_data" in m) && ("processMs" in m["save_data"]);
    }
    // Locate the measured messages by content, not by hardcoded index.
    // The tool_calls check comes first: the ToolMessage's content is JSON
    // null, so .str on it would throw.
    size_t a3Idx = size_t.max, tmIdx = size_t.max, trIdx = size_t.max;
    foreach (i, m; fileMsgs) {
        if ("tool_calls" in m)
            tmIdx = i;
        else if (m["role"].str == "tool" && m["name"].str == "query_semantic")
            trIdx = i;
        else if (m["role"].str == "assistant"
                && m["content"].type == JSONType.string && m["content"].str == "hello back")
            a3Idx = i;
    }
    assert(a3Idx != size_t.max && tmIdx != size_t.max && trIdx != size_t.max,
            "the three measured messages are in the file");
    assert(fileMsgs[a3Idx]["save_data"]["processMs"].integer == 432, "a3 keeps its 432 ms");
    assert(fileMsgs[tmIdx]["save_data"]["processMs"].integer == 120, "tm keeps its 120 ms");
    assert(fileMsgs[trIdx]["save_data"]["processMs"].integer == 42, "tr keeps its 42 ms");
    assert(!hasProcessMs(fileMsgs[0]) && !hasProcessMs(fileMsgs[1]), "a1/a2 were never measured");
    assert(fileMsgs[2]["role"].str == "user" && fileMsgs[2]["content"].str == "hello");
    assert(!hasProcessMs(fileMsgs[2]), "the user query was never measured");

    // Live stamps for the turn: a3 [4], tm [5], tr [6] (q consumes ordinal
    // 3 but is not displayed live - the echo is the user query's own job).
    assert(sink.stamps.length == 3, "only the measured turn traffic displays live");
    assert(sink.stamps.map!(s => s.nr).array == [4, 5, 6], "live turn ordinals");
    assert(sink.stamps[0].time.match!((Duration d) { return d == 432.dur!"msecs"; },
            (NoProcessingTime) { return false; }), "live a3 time");
    assert(sink.stamps[1].time.match!((Duration d) { return d == 120.dur!"msecs"; },
            (NoProcessingTime) { return false; }), "live tm time");
    assert(sink.stamps[2].time.match!((Duration d) { return d == 42.dur!"msecs"; },
            (NoProcessingTime) { return false; }), "live tr time");

    // Switch away and back: the replay shows the SAME [NR] and [TIME] -
    // the file content alone determines both (cross-instance property).
    sink.chatMessages.length = 0;
    sink.stamps.length = 0;
    app.switchToSession(metaB.id);
    assert(sink.stamps.length == 0, "empty session B replays nothing displayable");
    app.switchToSession(metaA.id);
    // Replay A: a1 [1], a2 [2], q [3] (user query now displayed),
    // a3 [4] 432 ms, tm [5] 120 ms, tr [6] 42 ms.
    assert(sink.stamps.length == 6, "all six displayable messages replayed");
    assert(sink.stamps.map!(s => s.nr).array == [1, 2, 3, 4, 5, 6],
            "replayed [NR] == live [NR] (persisted-NR property)");
    assert(sink.stamps[0].time.match!((Duration d) { return false; }, (NoProcessingTime) {
            return true;
        }), "a1 replays unmeasured");
    assert(sink.stamps[1].time.match!((Duration d) { return false; }, (NoProcessingTime) {
            return true;
        }), "a2 replays unmeasured");
    assert(sink.stamps[2].time.match!((Duration d) { return false; }, (NoProcessingTime) {
            return true;
        }), "q replays unmeasured");
    assert(sink.stamps[3].time.match!((Duration d) { return d == 432.dur!"msecs"; },
            (NoProcessingTime) { return false; }), "a3 replays its original 432 ms");
    assert(sink.stamps[4].time.match!((Duration d) { return d == 120.dur!"msecs"; },
            (NoProcessingTime) { return false; }), "tm replays its original 120 ms");
    assert(sink.stamps[5].time.match!((Duration d) { return d == 42.dur!"msecs"; },
            (NoProcessingTime) { return false; }), "tr replays its original 42 ms");
}

// Backward-compat gate: a session file created BEFORE the processMs
// feature (no save_data.processMs anywhere) must load and replay with
// contiguous [NR] 1..N and NO [TIME] suffix (NoProcessingTime - unmeasured is
// distinct from Duration(0)), and a later measured turn must commit WITHOUT
// giving the legacy messages a processMs key. NR is a pure derivation (the
// ordinal walk counts displayables), so there is nothing to migrate: if the
// test exposes a gap, the fix belongs in the layer that causes it.
@("legacy session files: contiguous NRs, no time suffix, clean round-trip")
unittest {
    import std.file : readText;
    import std.path : buildPath;
    import std.json : JSONValue, JSONType, parseJSON;
    import std.datetime : Duration, dur;
    import llm.agent : Agent;
    import llm.chat;
    import llm.session : SessionStore;
    import llm.types : ProcessResult;
    import my.filter : ReFilter;

    auto tmpDir = testArea("app_agent_nr_legacy");
    scope (exit)
        tmpDir.cleanup;

    auto cfg = testLlmConfig(tmpDir);
    auto store = new SessionStore(buildPath(tmpDir, "chat").Path);
    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.llmConf = cfg;
    app.sessionStore = store;
    app.agent_ = new Agent("main", cfg, null, null, null, ReFilter.init);
    auto sink = new TuiRecordingSink();
    app.uiMsg = new UiMessenger(sink);

    // Legacy doc: pre-feature shape - no save_data.processMs anywhere. The
    // save_data.user on the query is the pre-existing user-query marker, not
    // part of this feature.
    JSONValue doc;
    JSONValue lu;
    lu["role"] = "user";
    lu["content"] = "old question";
    lu["save_data"] = JSONValue(["user": JSONValue(true)]);
    JSONValue la1;
    la1["role"] = "assistant";
    la1["content"] = "old answer one";
    JSONValue la2;
    la2["role"] = "assistant";
    la2["content"] = "old answer two";
    doc["messages"] = JSONValue([lu, la1, la2]);
    auto meta = store.create();
    store.save(meta.id, meta, doc);
    auto throwawayMeta = store.create(); // a throwaway so the switch commits it
    app.activeSession = throwawayMeta;

    app.switchToSession(meta.id); // load + replay the legacy file

    // Contiguous NRs 1..3, ALL unmeasured (NoProcessingTime, not Duration(0)).
    assert(sink.stamps.length == 3);
    assert(sink.stamps.map!(s => s.nr).array == [1, 2, 3]);
    foreach (s; sink.stamps) {
        assert(s.time.match!((Duration d) { return false; }, (NoProcessingTime) {
                return true;
            }), "legacy messages must replay as NoProcessingTime");
    }

    // Round-trip: a new measured turn commits the session; the legacy messages
    // must not gain processMs. Mirror the production flow: the agent appends the
    // turn's messages to the chat BEFORE the step hook, so the batch is the
    // contiguous tail of the history.
    auto q = Message(Role.user, true, "new question", null);
    auto a1 = Message(Role.assistant, false, "new answer", null);
    a1.setProcessTime(MessageTime(250.dur!"msecs"));
    app.agent_.chat.add(q);
    app.agent_.chat.add(a1);
    ProcessResult result;
    result.chat = [Chat.MessageT(q), Chat.MessageT(a1)];
    app.processResult(result); // commits

    // The measured turn displays live at its true ordinal (5: lu,la1,la2,q,a1)
    // with its original 250 ms.
    assert(sink.stamps.length == 4, "the new answer displays live once");
    assert(sink.stamps[3].nr == 5, "the new answer's ordinal is its history position");
    assert(sink.stamps[3].time.match!((Duration d) { return d == 250.dur!"msecs"; },
            (NoProcessingTime) { return false; }), "the new answer keeps 250 ms");

    // Committed file: the legacy messages stayed pre-feature and the new message
    // is the ONLY one with processMs (no 0ms misrepresentation, no invented key).
    auto fileText = readText(buildPath(tmpDir, "chat", meta.id.get ~ ".json"));
    assert(fileText.canFind("processMs"), "the measured message persists processMs");
    auto fileJson = parseJSON(fileText);
    size_t nProcessMs = 0;
    long theValue = 0;
    foreach (m; fileJson["messages"].array) {
        if (m.type != JSONType.object || !("save_data" in m))
            continue;
        auto sd = m["save_data"];
        if (sd.type != JSONType.object || !("processMs" in sd))
            continue;
        nProcessMs++;
        theValue = sd["processMs"].integer;
    }
    assert(nProcessMs == 1, "exactly one message carries processMs (the new one)");
    assert(theValue == 250, "the new message's measured 250 ms round-trips; no 0ms");

    // Mixed-file reload: switch away and back; the
    // committed legacy+measured file must replay with contiguous NRs 1..5,
    // legacy messages + query unmeasured, a1 keeping its 250 ms.
    sink.chatMessages.length = 0;
    sink.stamps.length = 0;
    app.switchToSession(throwawayMeta.id);
    assert(sink.stamps.length == 0, "empty throwaway session replays nothing");
    app.switchToSession(meta.id); // reload the mixed committed file
    assert(sink.stamps.length == 5, "all five displayable messages replayed");
    assert(sink.stamps.map!(s => s.nr).array == [1, 2, 3, 4, 5],
            "replayed [NR] contiguous over the mixed file");
    assert(sink.stamps[0].time.match!((Duration d) { return false; }, (NoProcessingTime) {
            return true;
        }), "lu replays unmeasured");
    assert(sink.stamps[1].time.match!((Duration d) { return false; }, (NoProcessingTime) {
            return true;
        }), "la1 replays unmeasured");
    assert(sink.stamps[2].time.match!((Duration d) { return false; }, (NoProcessingTime) {
            return true;
        }), "la2 replays unmeasured");
    assert(sink.stamps[3].time.match!((Duration d) { return false; }, (NoProcessingTime) {
            return true;
        }), "q replays unmeasured");
    assert(sink.stamps[4].time.match!((Duration d) { return d == 250.dur!"msecs"; },
            (NoProcessingTime) { return false; }), "a1 replays its original 250 ms");
}
// Branch coverage: a failed or interrupted turn produces no displayable
// content, so it consumes no NR ordinal - and because NR is a pure derivation
// (recomputed from the persisted history at send time), the NEXT batch
// continues from the same offset: contiguous, no gap, no duplicate. There is
// no counter to desynchronize.
@("error turn: no stamps, next batch continues from the same offset")
unittest {
    import std.path : buildPath;
    import std.datetime : Duration, dur;
    import llm.agent : Agent;
    import llm.chat;
    import llm.session : SessionStore;
    import llm.types : ProcessResult;
    import my.filter : ReFilter;

    auto tmpDir = testArea("app_agent_nr_error");
    scope (exit)
        tmpDir.cleanup;

    auto cfg = testLlmConfig(tmpDir);
    auto store = new SessionStore(buildPath(tmpDir, "chat").Path);

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.llmConf = cfg;
    app.sessionStore = store;
    app.agent_ = new Agent("main", cfg, null, null, null, ReFilter.init);
    auto sink = new TuiRecordingSink();
    app.uiMsg = new UiMessenger(sink);
    app.activeSession = store.create();

    app.agent_.chat.setSystemPrompt("sys");
    app.agent_.chat.add(Message(Role.assistant, false, "a1", null));
    app.agent_.chat.add(Message(Role.assistant, false, "a2", null));
    // Direct chat.add is test-only: the production submit path sets the
    // dirty flag itself, so set it explicitly for the pre-turn commit.
    app.chatDirty = true;
    app.commitActiveSession();

    // A failed turn: nothing displayable arrives (user query persistence
    // is runAgent's job, not modeled here). The empty batch must not display
    // anything and must not advance any ordinal.
    ProcessResult failure;
    failure.status = ProcessResult.Status.unknownFailure;
    failure.chat = [];
    app.processResult(failure);
    assert(sink.stamps.length == 0, "error turn must not display anything");

    // The next batch continues from the SAME offset: a3 is the 4th
    // displayable in [sys, a1, a2, q, a3]. Mirror the production flow: the
    // agent appends the turn's messages before the step hook, so the batch
    // is the contiguous tail of the history.
    auto q = Message(Role.user, true, "hello", null);
    auto a3 = Message(Role.assistant, false, "hello back", null);
    a3.setProcessTime(MessageTime(432.dur!"msecs"));
    app.agent_.chat.add(q);
    app.agent_.chat.add(a3);
    ProcessResult result;
    result.chat = [Chat.MessageT(q), Chat.MessageT(a3)];
    app.processResult(result);
    assert(sink.stamps.length == 1);
    assert(sink.stamps[0].nr == 4, "next batch continues contiguously");
    assert(sink.stamps[0].time.match!((Duration d) { return d == 432.dur!"msecs"; },
            (NoProcessingTime) { return false; }));
}

version (unittest) {
    static import llm.test_util;

    /// Wire an AgentApp the way AppAgentActor.startSetup does for the fields
    /// rebuildSystemPrompt needs: test llmConf over `td` (including a base
    /// AGENT.md prompt file - getPrompt throws when it is missing), a live
    /// agent_, a skill manager, and an in-memory primary RAG (registered in
    /// the TestArea so cleanup destroys it before the dir is removed).
    private void wireRebuildTestApp(ref AgentApp app, ref llm.test_util.TestArea td) {
        import std.file : write;
        import std.path : buildPath;

        import my.filter : ReFilter;
        import llm.rag : RAG;
        import llm.test_util : TestArea, TestEmbedder;

        app.llmConf = testLlmConfig(td);
        // getPrompt(promptName: agentPrompt) reads promptDir/AGENT.md; the
        // startup prompt dir in production carries it.
        write(buildPath(app.llmConf.promptDir[0].toString, "AGENT.md"), "test agent prompt");
        app.skillManager_ = makeSkillManager(app.llmConf);
        app.noCwdConfig_ = false;
        // The AgentApp ctor leaves agent_ null (production creates it in
        // startSetup); the sibling tests assign it manually.
        app.agent_ = new Agent("main", app.llmConf, null, null, null, ReFilter.init);
        // A nonexistent primary dir makes openDatabase fall back to an
        // in-memory RAG (isPrimaryInMemory true) - the same gating fixture
        // pattern as llm.memory_tier. A literal ":memory:" path would be
        // mangled by AbsolutePath (resolved against the CWD) into a real,
        // process-wide shared file.
        auto rag = new RAG(new TestEmbedder(), RagDatabaseConfig(Path(buildPath(td,
                "no_such_dir", "agent_md_test.db")), "rebuild test rag"), null);
        td.addRag(rag);
        app.rag = rag;
    }

    // The system prompt at `history[0]` of a chat, or empty when the first
    // entry is not a system Message (loaded docs carry none before startup).
    // Shared by the lifecycle tests below; a unittest-local declaration would
    // not be visible across unittests.
    private string systemPromptOf(Chat.MessageT[] msgs) {
        import std.sumtype : match;

        import llm.chat : Message, Role, ToolMessage, ToolResponse, VisionMessage;

        string found;
        if (msgs.length == 0)
            return found;
        msgs[0].match!((Message a) {
            if (a.role == Role.system)
                found = a.content;
        }, (ToolMessage _) {}, (ToolResponse _) {}, (VisionMessage _) {});
        return found;
    }
}

@(
        "rebuildSystemPrompt rebuilds the startup prompt through the shared path (full treatment under the defaults)")
unittest {
    import std.file : readText;

    auto td = testArea("app_agent_rebuild_full");
    scope (exit)
        td.cleanup;

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    wireRebuildTestApp(app, td);

    // The verbatim-injection tests assert AGENTS.md injection, which the
    // full treatment only does while the repo AGENTS.md is under the
    // compression threshold; guard against the repo file growing past it.
    assert(readText("AGENTS.md").length <= 20 * 1024,
            "repo AGENTS.md exceeds the default 20 KB full-treatment threshold; these tests assume verbatim injection");

    app.rebuildSystemPrompt();

    // The real workarea AGENTS.md (20087 bytes, within the default 20 KB
    // threshold) rides along verbatim under the full treatment.
    assert(app.systemPrompt_.canFind(readText("AGENTS.md")),
            "full treatment injects the AGENTS.md payload verbatim");
    assert(app.systemPrompt_.canFind("test agent prompt"), "the base prompt file is still composed");
    assert(!app.systemPrompt_.canFind("For detailed explanations, architecture justifications"),
            "no RAG pointer under the full treatment");
    assert(app.rag.db.getSources.length == 1,
            "ragIndex indexes the document into the in-memory RAG");
    app.rag = null; // release app's reference so the TestArea owns the RAG
}

@("rebuildSystemPrompt defers while an agent run is in flight and applies at the next safe point")
unittest {
    import std.file : readText;

    auto td = testArea("app_agent_rebuild_defer");
    scope (exit)
        td.cleanup;

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    wireRebuildTestApp(app, td);

    app.agentRunInFlight_ = true;
    app.rebuildSystemPrompt();
    assert(app.agentMdRebuildPending_, "the rebuild is parked");
    assert(app.systemPrompt_.empty, "no prompt is composed while in flight");
    assert(app.rag.db.getSources.length == 0, "no AGENTS.md processing ran");

    app.agentRunInFlight_ = false;
    app.rebuildSystemPrompt();
    assert(!app.agentMdRebuildPending_, "the parked rebuild is applied");
    assert(app.systemPrompt_.canFind(readText("AGENTS.md")),
            "the applied rebuild injects the AGENTS.md payload");
    app.rag = null; // release app's reference so the TestArea owns the RAG
}

@(
        "rebuildSystemPrompt with agentsMd.enabled=false recomposes without the payload and without RAG interaction")
unittest {
    import std.file : readText;

    auto td = testArea("app_agent_rebuild_disabled");
    scope (exit)
        td.cleanup;

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    wireRebuildTestApp(app, td);

    app.llmConf.agentsMd.enabled = false;
    app.rebuildSystemPrompt();

    assert(app.systemPrompt_.canFind("test agent prompt"),
            "base prompt and manifest are still composed");
    assert(!app.systemPrompt_.canFind(readText("AGENTS.md")),
            "no AGENTS.md payload under enabled=false");
    assert(app.rag.db.getSources.length == 0, "no indexing runs");
    app.rag = null; // release app's reference so the TestArea owns the RAG
}

@("rebuildSystemPrompt with mode=full and ragIndex=false never touches the RAG")
unittest {
    import std.file : readText;

    auto td = testArea("app_agent_rebuild_no_rag");
    scope (exit)
        td.cleanup;

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    wireRebuildTestApp(app, td);

    app.llmConf.agentsMd.ragIndex = false;
    app.rebuildSystemPrompt();

    assert(app.systemPrompt_.canFind(readText("AGENTS.md")),
            "the payload is still injected under the full treatment");
    assert(app.rag.db.getSources.length == 0, "no RAG interaction at all");
}

@(
        "/refresh-agent-md with agentsMd.enabled=false reports the disabled status and performs no processing")
unittest {
    auto td = testArea("app_agent_refresh_disabled");
    scope (exit)
        td.cleanup;

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    wireRebuildTestApp(app, td);

    app.llmConf.agentsMd.enabled = false;
    SlashCommandRegistry reg;
    registerBuiltinCommands(reg);
    assert(reg.execute(app, "/refresh-agent-md") == AgentStatus.active);

    assert(app.rag.db.getSources.length == 0, "no processing runs under enabled=false");
    assert(!app.agentMdRebuildPending_, "no rebuild is parked");
    assert(app.systemPrompt_.empty, "the startup prompt is not composed");

    app.rag = null; // release app's reference so the TestArea owns the RAG
}

@("startup lifecycle: rebuildSystemPrompt composes the AGENTS.md payload through the shared startup path and installs it in the chat history")
unittest {
    import std.file : readText;

    auto td = testArea("app_agent_lifecycle_startup");
    scope (exit)
        td.cleanup;

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    wireRebuildTestApp(app, td);

    // This and the other lifecycle tests assert verbatim AGENTS.md injection,
    // which the full treatment only does while the repo AGENTS.md is under the
    // compression threshold; guard against the repo file growing past it.
    assert(readText("AGENTS.md").length <= 20 * 1024,
            "repo AGENTS.md exceeds the default 20 KB full-treatment threshold; these tests assume verbatim injection");

    app.rebuildSystemPrompt();

    assert(app.systemPrompt_.canFind(readText("AGENTS.md")),
            "the startup build injects the AGENTS.md payload verbatim");
    assert(app.systemPrompt_.canFind("test agent prompt"), "the base prompt file is still composed");
    assert(!app.systemPrompt_.canFind("For detailed explanations, architecture justifications"),
            "no RAG pointer under the full treatment");
    assert(systemPromptOf(app.agent_.chat.getMessages()) == app.systemPrompt_,
            "the composed prompt is installed at history[0]");
    size_t systemCount;
    foreach (m; app.agent_.chat.getMessages())
        m.match!((Message a) {
            if (a.role == Role.system)
                systemCount++;
        }, (ToolMessage _) {}, (ToolResponse _) {}, (VisionMessage _) {});
    assert(systemCount == 1, "exactly one system message");
    app.rag = null; // release app's reference so the TestArea owns the RAG
}

@("model-switch lifecycle: /model recomputes the prompt through the real dispatch path")
unittest {
    import std.file : readText;
    import std.typecons : Nullable;

    auto td = testArea("app_agent_lifecycle_model_switch");
    scope (exit)
        td.cleanup;

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    wireRebuildTestApp(app, td);

    // Model B opts out of AGENTS.md handling entirely (per-model override).
    app.llmConf.codeModels ~= CodeModelConfig(ServerConfig.init, "model-b", "model-b");
    AgentsMdOverride bOverride;
    bOverride.enabled = Nullable!bool(false);
    app.llmConf.codeModels[1].agentsMd = bOverride;

    app.rebuildSystemPrompt();
    assert(app.systemPrompt_.canFind(readText("AGENTS.md")),
            "setup: model A's full treatment injects the AGENTS.md payload");

    // Drive the REAL /model dispatch path: the registry routes the command to
    // modelHandler -> resetModel -> rebuildSystemPrompt.
    assert(app.slashCommands_.execute(app, "/model model-b") == AgentStatus.active,
            "/model <name> must dispatch as an in-session command");
    assert(app.llmConf.activeCodeModelIndex == 1, "the active model is now model-b");

    // The recomputed prompt applies model B's treatment: disabled -> no payload.
    assert(!app.systemPrompt_.canFind(readText("AGENTS.md")),
            "the recomputed prompt has no AGENTS.md payload under model B's disabled treatment");
    assert(!app.systemPrompt_.canFind("For detailed explanations, architecture justifications"),
            "no RAG pointer without a payload");
    assert(app.systemPrompt_.canFind("test agent prompt"), "the base prompt file is still composed");
    assert(systemPromptOf(app.agent_.chat.getMessages()) == app.systemPrompt_,
            "the recomputed prompt is installed in the chat history");
    app.rag = null; // release app's reference so the TestArea owns the RAG
}

@("refresh-agent-md lifecycle: the disabled path reports and does nothing; the enabled path clears the cache and reprocesses")
unittest {
    import std.file : exists, readText, write;

    auto td = testArea("app_agent_lifecycle_refresh");
    scope (exit)
        td.cleanup;

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    wireRebuildTestApp(app, td);
    app.llmConf.dataDir = td.workArea; // the cache lives in the workarea
    auto cachePath = td.workArea ~ "agent_md_cache.json";
    SlashCommandRegistry reg;
    registerBuiltinCommands(reg);

    // First the disabled path: a pure no-op that reports a status message.
    app.llmConf.agentsMd.enabled = false;
    assert(reg.execute(app, "/refresh-agent-md") == AgentStatus.active,
            "/refresh-agent-md must dispatch as an in-session command");
    assert(app.rag.db.getSources.length == 0, "no processing runs under enabled=false");
    assert(!app.agentMdRebuildPending_, "no rebuild is parked");
    assert(app.systemPrompt_.empty, "the startup prompt is not composed");
    assert(!cachePath.exists, "no cache file is created under enabled=false");

    // Then re-enable, force the full treatment, and seed a stale cache
    // entry so the refresh has something to clear.
    app.llmConf.agentsMd.enabled = true;
    app.llmConf.agentsMd.compression.mode = CompressionMode.full;
    write(cachePath.toString, `{"checksum":1,"summary":"- stale cached summary\n","timestamp":1}`);
    assert(reg.execute(app, "/refresh-agent-md") == AgentStatus.active);

    // forceRefresh clears the cache file up front, whatever the treatment;
    // mode=full then injects the payload verbatim and indexes it into the
    // in-memory RAG (ragIndex=true).
    assert(!cachePath.exists, "the forced refresh clears the cache file");
    assert(app.systemPrompt_.canFind(readText("AGENTS.md")),
            "the forced refresh injects the AGENTS.md payload verbatim");
    assert(!app.systemPrompt_.canFind("- stale cached summary"),
            "no stale cached summary survives the refresh");
    assert(!app.systemPrompt_.canFind("For detailed explanations, architecture justifications"),
            "no RAG pointer under the full treatment");
    assert(app.systemPrompt_.canFind("test agent prompt"), "the base prompt file is still composed");
    assert(app.rag.db.getSources.length == 1,
            "mode=full indexes the verbatim payload into the in-memory RAG");
    app.rag = null; // release app's reference so the TestArea owns the RAG
}

@("session-switch lifecycle: switchToSession re-applies the current prompt after the chat drifted")
unittest {
    import std.sumtype : match;

    import llm.chat : Message, Role, ToolMessage, ToolResponse, VisionMessage;

    auto td = testArea("app_agent_lifecycle_session_switch");
    scope (exit)
        td.cleanup;

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    wireRebuildTestApp(app, td);
    app.rebuildSystemPrompt();
    immutable recomputed = app.systemPrompt_;
    assert(systemPromptOf(app.agent_.chat.getMessages()) == recomputed,
            "setup: the startup prompt is installed in the chat history");

    // A real SessionStore over a directory inside the test area, so the
    // session switch persists and loads through the store like production.
    app.sessionStore = new SessionStore((td.workArea ~ "chat").Path);

    auto metaA = app.sessionStore.create();
    app.switchToSession(metaA.id);
    assert(systemPromptOf(app.agent_.chat.getMessages()) == recomputed,
            "setup: activating session A keeps the startup prompt");

    // The chat drifts off the startup prompt before the switch.
    app.agent_.setSystemPrompt("stale session prompt");
    assert(systemPromptOf(app.agent_.chat.getMessages()) == "stale session prompt",
            "setup: the prompt at history[0] must be stale before the switch");

    auto metaB = app.sessionStore.create();
    app.switchToSession(metaB.id);
    assert(systemPromptOf(app.agent_.chat.getMessages()) == recomputed,
            "activating session B must restore the startup prompt");

    app.switchToSession(metaA.id);
    assert(systemPromptOf(app.agent_.chat.getMessages()) == recomputed,
            "activating session A again must restore the startup prompt");
    assert(systemPromptOf(app.agent_.chat.getMessages()) != "stale session prompt",
            "the stale prompt must not survive the switch");

    // Exactly one system message: the re-apply must not duplicate prompts.
    size_t systemCount;
    foreach (m; app.agent_.chat.getMessages())
        m.match!((Message a) {
            if (a.role == Role.system)
                systemCount++;
        }, (ToolMessage _) {}, (ToolResponse _) {}, (VisionMessage _) {});
    assert(systemCount == 1, "the re-apply must not add a second system message");

    app.rag = null; // release app's reference so the TestArea owns the RAG
}

@("injection rows end-to-end: the real path produces each payload/instruction combination")
unittest {
    import std.datetime.systime : Clock;
    import std.file : exists, readText, remove;
    import std.string : format, indexOf;

    auto td = testArea("app_agent_lifecycle_rows");
    scope (exit)
        td.cleanup;

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    wireRebuildTestApp(app, td);
    // The cache and the summary prompt both live in the test area: dataDir
    // selects the cache file, and removing SUMMARY.md forces the summarization
    // failure deterministically (no LLM endpoint is contacted).
    app.llmConf.dataDir = td.workArea;
    auto cachePath = td.workArea ~ "agent_md_cache.json";
    const rawChecksum = computeContentHash(readText("AGENTS.md"));
    const topicName = format!"agent_md_%08x"(rawChecksum);
    const generatedSummary = "- stubbed AGENTS.md summary for the lifecycle rows\n";

    // Row A: summary treatment + ragIndex=true -> payload (the generated
    // summary) plus the RAG pointer instruction naming the raw checksum topic.
    app.llmConf.agentsMd.compression.mode = CompressionMode.summary;
    AgentMdState seeded;
    seeded.checksum = rawChecksum;
    seeded.summary = generatedSummary;
    seeded.timestamp = Clock.currTime();
    assert(saveAgentMdCache(cachePath, seeded), "setup: the cache file must be seeded");
    app.rebuildSystemPrompt();
    assert(app.systemPrompt_.canFind(generatedSummary),
            "row A: the summary treatment injects the cached summary");
    assert(app.systemPrompt_.canFind("For detailed explanations, architecture justifications"),
            "row A: the RAG pointer instruction rides along");
    assert(app.systemPrompt_.canFind("query your RAG knowledge base for topic `" ~ topicName ~ "`"),
            "row A: the pointer names the raw checksum topic");
    assert(app.systemPrompt_.indexOf(generatedSummary) < app.systemPrompt_.indexOf(
            "For detailed explanations, architecture justifications"),
            "row A: the payload precedes the pointer instruction");
    assert(app.rag.db.getSources.length == 1,
            "row A: the pointer's topic is reloaded into the in-memory RAG");

    // Row B: summary treatment + ragIndex=false -> payload without pointer.
    app.llmConf.agentsMd.ragIndex = false;
    app.rebuildSystemPrompt();
    assert(app.systemPrompt_.canFind(generatedSummary),
            "row B: the cached summary is still the payload");
    assert(!app.systemPrompt_.canFind("For detailed explanations, architecture justifications"),
            "row B: no RAG pointer under ragIndex=false");
    // The reload is gated on ragIndex (agent_md.d), so the row-A topic
    // simply persists in the ephemeral RAG - no new interaction, none
    // removed.
    assert(app.rag.db.getSources.length == 1,
            "row B: ragIndex=false adds no topic and removes none");

    // Row C: summarization fails with no valid cache -> empty payload.
    remove(cachePath.toString);
    remove((app.llmConf.promptDir[0] ~ "SUMMARY.md").toString);
    app.rebuildSystemPrompt();
    assert(app.systemPrompt_.canFind("test agent prompt"),
            "row C: the base prompt file is still composed");
    assert(!app.systemPrompt_.canFind(generatedSummary),
            "row C: no payload after the summarization failure");
    assert(!app.systemPrompt_.canFind("For detailed explanations, architecture justifications"),
            "row C: no pointer without a payload");
    assert(!cachePath.exists, "row C: the failure path saves no cache file");
    app.rag = null; // release app's reference so the TestArea owns the RAG
}

@("rebuild deferral lifecycle: a rebuild requested while an agent run is in flight is parked and applied by the next safe point after the run")
unittest {
    import std.file : readText;
    import std.typecons : Nullable;

    auto td = testArea("app_agent_lifecycle_defer");
    scope (exit)
        td.cleanup;

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    wireRebuildTestApp(app, td);

    // Model B opts out of AGENTS.md handling entirely (per-model override).
    app.llmConf.codeModels ~= CodeModelConfig(ServerConfig.init, "model-b", "model-b");
    AgentsMdOverride bOverride;
    bOverride.enabled = Nullable!bool(false);
    app.llmConf.codeModels[1].agentsMd = bOverride;

    assert(readText("AGENTS.md").length <= 20 * 1024,
            "repo AGENTS.md exceeds the default 20 KB full-treatment threshold; these tests assume verbatim injection");

    app.rebuildSystemPrompt();
    assert(app.systemPrompt_.canFind(readText("AGENTS.md")),
            "setup: model A's full treatment injects the AGENTS.md payload");

    // Park a rebuild the way the in-flight guard does: the production safe
    // point (runAgent's post-run block) clears the in-flight flag and calls
    // rebuildSystemPrompt; the guard itself only parks.
    app.agentRunInFlight_ = true;
    assert(app.slashCommands_.execute(app, "/model model-b") == AgentStatus.active,
            "/model must dispatch as an in-session command");
    assert(app.agentMdRebuildPending_, "the rebuild is parked while the run is in flight");
    assert(app.llmConf.activeCodeModelIndex == 1, "the model switch itself is not deferred");
    assert(app.systemPrompt_.canFind(readText("AGENTS.md")),
            "model A's payload is still applied while the rebuild is parked");

    // The next safe point applies the parked rebuild.
    app.agentRunInFlight_ = false;
    app.rebuildSystemPrompt();
    assert(!app.agentMdRebuildPending_, "the parked rebuild is applied");
    assert(!app.systemPrompt_.canFind(readText("AGENTS.md")),
            "the applied rebuild switches to model B's disabled treatment");
    assert(systemPromptOf(app.agent_.chat.getMessages()) == app.systemPrompt_,
            "the recomputed prompt is installed in the chat history");
    app.rag = null; // release app's reference so the TestArea owns the RAG
}

// A new session is its own confirmation: it becomes the active rail row and the
// thread is empty. A synthetic assistant turn here renders as an "agent - 1 step"
// work group and buries the real transcript (GUI report), so the create path must
// post no chat message at all.
@("creating a session posts no chat message")
unittest {
    import std.path : buildPath;
    import llm.agent : Agent;
    import llm.session : SessionStore;
    import my.filter : ReFilter;

    auto tmpDir = testArea("app_agent_create_session_silent");
    scope (exit)
        tmpDir.cleanup;

    auto cfg = testLlmConfig(tmpDir);
    auto store = new SessionStore(buildPath(tmpDir, "chat").Path);
    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    app.llmConf = cfg;
    app.sessionStore = store;
    // activateSession() drives the agent's chat, exactly as the actor does.
    app.agent_ = new Agent("main", cfg, null, null, null, ReFilter.init);
    auto sink = new TuiRecordingSink();
    app.uiMsg = new UiMessenger(sink);

    app.doCreateSession();

    assert(sink.chatMessages.length == 0, "a new session must not post a chat turn");
    assert(store.list().length == 1, "the session was created");
    assert(app.activeSession.id.length > 0, "the new session is active");
}

@("the pipeline updater sends the context numbers and the newest tool as fields")
unittest {
    auto sink = new TuiRecordingSink();
    auto ui = new UiMessenger(sink);
    auto updater = new PipelineStreamMessageUpdater(ui, 8192, "model-x");
    updater.setId("coder");

    // No tool call yet: no tool name, and the numbers are the stat's (startContext + tokenCount).
    updater.messageUpdate(StreamMessage(content: "hello"), null,
            ServerStat(startContext: 1200, tokenCount: 34));
    assert(sink.laneUpdates.length == 1, "one tick, one recorded lane update");
    auto first = sink.laneUpdates[0];
    assert(first.agentId == "coder", "the agent id travels");
    assert(first.content.canFind("hello"), "the stream content still crosses");
    assert(first.currentTool.length == 0, "no tool call yet: the field is empty");
    assert(first.contextUsed == 1234, "contextUsed is stat.context: " ~ first
            .contextUsed.to!string);
    assert(first.contextMax == 8192, "contextMax is the model's context size");
    assert(first.status.canFind("1234/8192 tokens"),
            "the legacy sentence still goes out (the legacy panel paints it): " ~ first.status);

    // Two calls in one response: the array arrives in protocol order, so the last element is the
    // newest call - the tool name the row shows.
    StreamToolCall[] tools = [
        StreamToolCall(toolName: "grep_files", arguments: "{}"),
        StreamToolCall(toolName: "write_file", arguments: "{\"path\":\"/tmp/x\"}")
    ];
    updater.messageUpdate(StreamMessage(content: "done", role: "assistant"),
            tools, ServerStat(startContext: 10, tokenCount: 5));
    assert(sink.countOf("pipelineStreamChatMessage") == 2, "one message per tick");
    auto last = sink.laneUpdates[$ - 1];
    assert(last.currentTool == "write_file", "the newest call (highest index) is the field");
    assert(last.contextUsed == 15, "the numbers move with the stat");
    assert(last.role == "assistant", "the role crosses");
    assert(last.content.canFind("--- Tool ---"),
            "the marker blocks are still written into the content (the legacy panel reads them)");
    assert(last.content.canFind("grep_files")
            && last.content.canFind("write_file"), "both tool blocks are in the text");
}
