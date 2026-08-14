/// Session-management slash commands: /sessions, /switch, /new, /rename,
/// /delete, /clear. Also owns the /delete confirmation state machine (W3/W9:
/// PendingDeleteAction and decideDeleteCommand).
module llm.app_agent.slash_session;

import std.array : empty;
import std.conv : to;
import std.exception : ifThrown;
import std.functional : toDelegate;
import std.string : strip;

import llm.app_agent; // AgentApp (cyclic package import, same pattern as slash.d)
import llm.app_agent.slash;
import llm.session : SessionId, resolveSessionRef;
import llm.types : ServerStat;
import llmfun_tui; // TuiChatMessageType_* (C binding)
import my.optional : hasValue, orElse;

/// Result of deciding a `/delete` command against the pending confirmation
/// state. Package-visible: the dispatcher-level pending-delete rule and the
/// tests in `tests.d` read it; outside the package it is hidden.
package enum PendingDeleteAction {
    ignore, // no pending confirmation
    confirm, // resolved id matches the pending id
    clear // pending exists but a different id was given
}

package void registerSessionCommands(ref SlashCommandRegistry registry) {
    auto ignore = registry.register(SlashCommand("sessions", [],
            [
                "   /sessions          List chat sessions (index, id, title, preview)"
    ], SlashArgMode.none, 50, toDelegate(&sessionsHandler)));
    ignore = registry.register(SlashCommand("switch", [], [
        "   /switch <n|id|title>  Switch to another session"
    ], SlashArgMode.required, 60, toDelegate(&switchHandler)));
    ignore = registry.register(SlashCommand("new", [], [
        "   /new               Start a new chat session"
    ], SlashArgMode.none, 70, toDelegate(&newHandler)));
    ignore = registry.register(SlashCommand("rename", [], [
        "   /rename <title>    Rename the current session"
    ], SlashArgMode.required, 80, toDelegate(&renameHandler)));
    ignore = registry.register(SlashCommand("delete", [],
            ["   /delete <n>        Delete a session (repeat to confirm)"],
            SlashArgMode.optional, 90, toDelegate(&deleteHandler)));
    ignore = registry.register(SlashCommand("clear", [], [
        "   /clear             Clear the current chat history"
    ], SlashArgMode.none, 100, toDelegate(&clearHandler)));
}

/** Decide how a `/delete` command proceeds given the pending state (pure).
 *
 * Params:
 *   pendingId = session id awaiting confirmation (SessionId.init = none)
 *   resolvedId = id resolved from this command's argument
 *
 * Returns: confirm when the ids match, clear when a different id is
 *          given, ignore when there is no pending confirmation.
 */
package PendingDeleteAction decideDeleteCommand(SessionId pendingId, SessionId resolvedId) @safe pure nothrow {
    if (pendingId.length == 0)
        return PendingDeleteAction.ignore;
    if (pendingId == resolvedId)
        return PendingDeleteAction.confirm;
    return PendingDeleteAction.clear;
}

/// `/sessions`: list all sessions.
private AgentStatus sessionsHandler(ref AgentApp app, string arg) {
    app.doListSessions();
    return AgentStatus.active;
}

/// `/new`: create a fresh session and switch to it.
private AgentStatus newHandler(ref AgentApp app, string arg) {
    app.doCreateSession();
    return AgentStatus.active;
}

/// `/switch <arg>`: resolve the target (index, id, or case-insensitive title)
/// and switch to it (the argMode `required` rule supplies the bare-command).
private AgentStatus switchHandler(ref AgentApp app, string arg) {
    auto stripped = arg.strip;
    if (stripped.empty) {
        app.sendChatMessage(
                "error: /switch requires an argument. Usage: /switch <index|id|title>. Use /sessions to list.",
                TuiChatMessageType_Assistant);
    } else {
        auto sessions = app.sessionStore.list();
        if (sessions.empty) {
            app.sendChatMessage("No sessions available. Use /new to create one.",
                    TuiChatMessageType_Assistant);
        } else {
            auto resolved = resolveSessionRef(sessions, stripped);
            if (hasValue(resolved)) {
                auto id = orElse(resolved, SessionId.init);
                app.switchToSession(id);
            } else {
                app.sendChatMessage("error: Unknown session '%s'. Use /sessions to list available sessions.",
                        TuiChatMessageType_Assistant, stripped);
            }
        }
    }
    return AgentStatus.active;
}

/// `/rename <arg>`: rename the active session (the argMode `required` rule
/// supplies the bare-command).
private AgentStatus renameHandler(ref AgentApp app, string arg) {
    auto stripped = arg.strip;
    if (stripped.empty) {
        app.sendChatMessage("error: /rename requires a title argument. Usage: /rename <title>.",
                TuiChatMessageType_Assistant);
    } else {
        app.doRenameSession(stripped);
    }
    return AgentStatus.active;
}

/// `/delete [arg]`: confirm-then-delete state machine. The
/// dispatcher-level rule "any input not starting with /delete clears the
/// pending id" stays in runAgent.
private AgentStatus deleteHandler(ref AgentApp app, string arg) {
    auto stripped = arg.strip;
    if (stripped.empty) {
        app.sendChatMessage("error: /delete requires an index. Usage: /delete <n>.",
                TuiChatMessageType_Assistant);
        app.pendingDeleteId = SessionId.init;
        return AgentStatus.active;
    }
    auto idx = ifThrown(stripped.to!long, -1L);
    auto sessions = app.sessionStore.list();
    if (idx < 1 || idx > cast(long) sessions.length) {
        app.sendChatMessage("error: Unknown session index '%s'. Use /sessions to list available sessions.",
                TuiChatMessageType_Assistant, stripped);
        app.pendingDeleteId = SessionId.init;
        return AgentStatus.active;
    }
    auto resolvedId = sessions[cast(size_t)(idx - 1)].id;
    final switch (decideDeleteCommand(app.pendingDeleteId, resolvedId)) {
    case PendingDeleteAction.ignore:
        app.pendingDeleteId = resolvedId;
        app.sendChatMessage("Confirm deletion of session '%s' (%s, %s msgs) by repeating /delete %s.",
                TuiChatMessageType_Assistant,
                app.shortSessionId(resolvedId), sessions[cast(size_t)(idx - 1)].title,
                sessions[cast(size_t)(idx - 1)].messageCount, stripped);
        break;
    case PendingDeleteAction.confirm:
        app.pendingDeleteId = SessionId.init;
        app.doDeleteSession(resolvedId);
        break;
    case PendingDeleteAction.clear:
        app.pendingDeleteId = SessionId.init;
        app.sendChatMessage("Deletion cancelled (different session). Repeat /delete <n> to start over.",
                TuiChatMessageType_Assistant);
        break;
    }
    return AgentStatus.active;
}

/// `/clear`: explicit in-session history wipe.
private AgentStatus clearHandler(ref AgentApp app, string arg) {
    // Old /new behavior: explicit in-session wipe (F11). Order matters:
    // clearHistory -> UI clear -> context reset -> pipelineClear -> save.
    app.agent_.clearHistory(); // keeps system prompt at history[0]
    app.uiMsg.clearChat();
    app.lastServerStat = ServerStat(startContext: 0); // context resets to 0
    app.uiMsg.pipelineClear();
    app.commitActiveSession();
    app.sendChatMessage("Cleared chat history in session '%s'.",
            TuiChatMessageType_Assistant, app.shortSessionId(app.activeSession.id));
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
    registerSessionCommands(reg);

    // SlashArgMode.none: exact match only — any arg takes the unknown path
    assert(reg.execute(app, "/sessions now") == AgentStatus.active);
    assert(reg.execute(app, "/new please") == AgentStatus.active);
    assert(reg.execute(app, "/clear please") == AgentStatus.active);

    // SlashArgMode.required: bare /switch and /rename take the unknown path (W1)
    assert(reg.execute(app, "/switch") == AgentStatus.active);
    assert(reg.execute(app, "/rename") == AgentStatus.active);

    // SlashArgMode.optional: bare /delete dispatches to the handler, which
    // emits its usage error (no agent_/sessionStore dereference).
    assert(reg.execute(app, "/delete") == AgentStatus.active);

    // The handlers with side effects are not dispatched here: they
    // dereference agent_ / sessionStore (only created in run()).

    // Help lines are the verbatim strings
    auto help = reg.helpText();
    assert(help.canFind("   /sessions          List chat sessions (index, id, title, preview)"));
    assert(help.canFind("   /switch <n|id|title>  Switch to another session"));
    assert(help.canFind("   /new               Start a new chat session"));
    assert(help.canFind("   /rename <title>    Rename the current session"));
    assert(help.canFind("   /delete <n>        Delete a session (repeat to confirm)"));
    assert(help.canFind("   /clear             Clear the current chat history"));

    // Help ordering is (order asc, registration index asc): 50, 60, 70, 80, 90, 100
    assert(help.indexOf("   /sessions") < help.indexOf("   /switch"));
    assert(help.indexOf("   /switch") < help.indexOf("   /new"));
    assert(help.indexOf("   /new") < help.indexOf("   /rename"));
    assert(help.indexOf("   /rename") < help.indexOf("   /delete"));
    assert(help.indexOf("   /delete") < help.indexOf("   /clear"));
}

// --- Test: decideDeleteCommand state machine ---

unittest {
    // No pending confirmation -> ignore
    assert(decideDeleteCommand(SessionId.init, SessionId("idA")) == PendingDeleteAction.ignore);
    assert(decideDeleteCommand(SessionId.init, SessionId.init) == PendingDeleteAction.ignore);

    // Same id -> confirm
    assert(decideDeleteCommand(SessionId("idA"), SessionId("idA")) == PendingDeleteAction.confirm);

    // Different id -> clear
    assert(decideDeleteCommand(SessionId("idA"), SessionId("idB")) == PendingDeleteAction.clear);
    assert(decideDeleteCommand(SessionId("idA"), SessionId.init) == PendingDeleteAction.clear);
}
