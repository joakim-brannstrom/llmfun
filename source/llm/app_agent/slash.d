/// Slash command registry: the plugin machinery behind AgentApp's slash commands.
module llm.app_agent.slash;

import core.attribute : mustuse;
import core.sync : Mutex;
import logger = std.logger;
import std.algorithm.mutation : SwapStrategy;
import std.algorithm.sorting : sort;
import std.array : empty, join;
import std.format : format;
import std.string : indexOf, leftJustify, startsWith;
import std.exception : collectException;

import llm.app_agent;
import llm.app_agent.slash_core;
import llm.app_agent.slash_model;
import llm.app_agent.slash_pipeline;
import llm.app_agent.slash_session;
import llm.app_agent.slash_skills;

import llmfun_tui; // TuiChatMessageType / TuiChatMessageType_Assistant (C++ binding)

/// How a command consumes its argument (drives dispatch + help rendering).
enum SlashArgMode {
    none,
    required,
    optional
}

/// Outcome of running an agent input: keep going or shut down.
enum AgentStatus {
    active,
    terminate
}

/// One registered slash command.
struct SlashCommand {
    string name; // canonical name, no leading '/', e.g. "quit"
    string[] aliases; // e.g. ["q", "exit"]
    string[] helpLines; // complete pre-formatted help lines
    SlashArgMode argMode;
    int order; // ascending order controls help listing position
    AgentStatus delegate(ref AgentApp app, string arg) handler;
}

/// Registry of slash commands. Value-type field of `AgentApp`; handlers are
/// free functions taking `ref AgentApp` — no delegate captures, no `this`
/// capture, no lifetime hazard.
struct SlashCommandRegistry {
    private SlashCommand[string] byName_; // canonical + aliases → command (O(1) lookup)
    private SlashCommand[] ordered_; // registration order; tiebreak key for help output

    @mustuse static struct Status {
        bool status;
    }

    /// Registers a command; throws on duplicate canonical name or alias.
    /// All validation happens before any mutation: after a throw the registry
    /// is exactly as it was — a partially-registered command would be
    /// unrecoverable (dispatchable via byName_ but invisible to helpText).
    Status register(SlashCommand cmd) {
        if (cmd.name.empty) {
            logger.warning("Slash command name must not be empty");
            return Status(false);
        }
        if (cmd.handler is null) {
            logger.warningf("Slash command '%s' has no handler", cmd.name);
            return Status(false);
        }
        if (cmd.name in byName_) {
            logger.warningf("Slash command '%s' is already registered", cmd.name);
            return Status(false);
        }
        foreach (alias_; cmd.aliases) {
            if (alias_ == cmd.name) {
                logger.warningf("Slash command '%s': alias '%s' equals the canonical name",
                        cmd.name, alias_);
                return Status(false);
            }
            if (alias_ in byName_) {
                logger.warningf("Slash command alias '%s' (for '%s') is already registered",
                        alias_, cmd.name);
                return Status(false);
            }
        }

        byName_[cmd.name] = cmd;
        foreach (alias_; cmd.aliases)
            byName_[alias_] = cmd;
        ordered_ ~= cmd;
        return Status(true);
    }

    /** Dispatch an input starting with '/' (asserted; the dispatcher checks
     * `isSlashCommand` first).
     *
     * Tokenization: body = input without the leading '/'; name = body up to
     * the first space (whole body when there is no space); arg = raw remainder
     * after the first space, unstripped ("" when there is no space). Unknown
     * commands and argMode violations take the verbatim unknown-command path.
     *
     * NOTE: the pending-delete clearing rule lives in the dispatcher
     * , NOT here — direct callers of `execute` must apply it
     * themselves.
     */
    AgentStatus execute(ref AgentApp app, string input) const nothrow
    in (input.length > 0 && input[0] == '/', "execute expects an input starting with '/'") {
        auto name = commandNameOf(input);
        auto sp = input.indexOf(' ');
        auto arg = sp == -1 ? "" : input[sp + 1 .. $];

        auto pcmd = name in byName_; // AA lookup returns a pointer (W6)
        if (pcmd is null) {
            unknownCommand(app, input);
            return AgentStatus.active;
        }
        auto cmd = *pcmd;
        if (cmd.argMode == SlashArgMode.none && !arg.empty) {
            unknownCommand(app, input); // exact-match semantics
            return AgentStatus.active;
        }
        if (cmd.argMode == SlashArgMode.required && arg.empty) {
            unknownCommand(app, input); // W1: bare /plan, /code, /switch, /rename
            return AgentStatus.active;
        }
        try {
            return cmd.handler(app, arg);
        } catch (Exception e) {
            logger.warningf("slash handler threw on user input '%s' with error: %s",
                    input, e.msg).collectException;
        }
        return AgentStatus.active; // assuming agent is active even though a slash command failed
    }

    /// Returns: True when the input starts with '/' (the dispatcher routes it here).
    bool isSlashCommand(string input) const @safe pure nothrow {
        return input.length > 0 && input[0] == '/';
    }

    /** True when the input's command name (tokenized exactly like `execute`
     * via `commandNameOf`) is registered. Package-visible: the dispatcher
     * (package.d) uses it to preserve the pending-delete clear for
     * /delete-prefixed non-commands like `/deletefoo` without reaching into
     * the registry's private `byName_`. Non-slash input never matches (bare
     * words are not command inputs; the dispatcher only queries after
     * `isSlashCommand`).
     */
    package bool isRegistered(string input) const {
        if (input.empty || input[0] != '/')
            return false;
        return (commandNameOf(input) in byName_) !is null;
    }

    /** Command name of a slash input: body up to the first space (the whole
     * body when there is no space). Shared by `execute` and `isRegistered`
     * so the tokenization cannot drift.
     */
    private static string commandNameOf(string input) @safe nothrow {
        auto body = input.length > 0 && input[0] == '/' ? input[1 .. $] : input;
        auto sp = body.indexOf(' ');
        return sp == -1 ? body : body[0 .. sp];
    }

    /** Render the `/help` text: header + bare-query line + every command's
     * help lines, sorted stably by `(order asc, registration index asc)` (W8).
     * The golden test in `tests.d` locks the exact byte-for-byte output.
     *
     * Sorting uses an index array: `SlashCommand` holds a delegate, so
     * `const(SlashCommand)[] .dup` is not copyable (const delegate cannot
     * convert back to mutable); indices are plain value types.
     */
    string helpText() const {
        string[] lines;
        lines ~= "llmfun agent mode - type a query and press Tab to start.";
        lines ~= " Use /commands for special actions:";
        lines ~= "";
        lines ~= "   (bare query)       Send a message to the agent";

        size_t[] idx;
        foreach (i; 0 .. ordered_.length)
            idx ~= i;
        idx.sort!((a, b) => ordered_[a].order < ordered_[b].order, SwapStrategy.stable);
        foreach (i; idx)
            lines ~= ordered_[i].helpLines;
        return lines.join("\n");
    }

    /** ArgMode of a registered command by canonical name.
     *
     * Package-visible introspection: lets tests assert a command's argMode
     * directly instead of inferring it from dispatch behavior (which only
     * discriminates via the unknown-command path). Returns `SlashArgMode.none`
     * for an unregistered name — the enum's default, indistinguishable from a
     * registered `none` command; callers asserting on a known command are
     * unaffected.
     */
    package SlashArgMode argModeOf(string name) const {
        auto pcmd = name in byName_; // AA lookup returns a pointer (W6)
        return pcmd is null ? SlashArgMode.none : pcmd.argMode;
    }

    /** Format a plugin help line using the W4 padding formula: descriptions
     * start at column 22 for usage ≤ 19 chars, with a two-space gap (column
     * 25) only when usage exceeds 19.
     */
    static string formatHelpLine(string usage, string description) {
        return "   " ~ usage.leftJustify(19) ~ (usage.length > 19 ? "  " : "") ~ description;
    }

    /// Unknown-command message. Deliberately does NOT clear `pendingDeleteId`:
    /// the pending-delete rule belongs to the dispatcher,
    /// which compensates for /delete-prefixed unknowns before calling
    /// `execute`. Direct `execute` callers must apply that rule themselves.
    private static void unknownCommand(ref AgentApp app, string input) nothrow {
        try {
            app.sendChatMessage("system: Unknown command: '%s'. Type /help for available commands.",
                    TuiChatMessageType_Assistant, input);
        } catch (Exception e) {
        }
    }
}

/// Registers every built-in command group. Called by AgentApp's
/// constructor before the startup-hook commands. The five group
/// registrars are package-visible in this package.
void registerBuiltinCommands(ref SlashCommandRegistry registry) {
    registerCoreCommands(registry);
    registerSessionCommands(registry);
    registerModelCommands(registry);
    registerPipelineCommands(registry);
    registerSkillsCommands(registry);
}

// Startup command hook for external plugins: append-only list
// consumed by AgentApp's constructor after the built-ins. External plugin
// modules append in their module `static this()`; D guarantees module
// constructors run before `main`, so ordering is safe with no static-ctor
// coupling in this module (neither slash.d nor package.d adds static ctors).
private shared SlashCommand[] startupCommands_;
private shared Mutex mtx; // guards startupCommands_

shared static this() {
    mtx = cast(shared) new Mutex;
}

/// Startup command hook for external plugins.
void addStartupSlashCommand(SlashCommand cmd) {
    mtx.lock_nothrow();
    scope (exit)
        mtx.unlock_nothrow();
    startupCommands_ ~= cast(shared) cmd;
}

/// Startup commands registered via `addStartupSlashCommand`, read by
/// AgentApp's constructor after the built-ins.
/// Returns: a copy: the module-global list stays append-only — a caller
/// mutating the slice must not corrupt the source of truth.
package SlashCommand[] startupSlashCommands() {
    mtx.lock_nothrow();
    scope (exit)
        mtx.unlock_nothrow();
    return cast(SlashCommand[]) startupCommands_.dup;
}

unittest {
    import llm.app_config : UserConfig;
    import std.exception : assertThrown;

    // Real AgentApp with a blocked UiMessenger (W5): execute's unknown path
    // calls sendChatMessage, which routes to writeln in blocked mode — never
    // a null uiMsg dereference.
    auto app = AgentApp(UserConfig.AgentChatConfig.init);

    SlashCommandRegistry reg;
    assert(reg.register(SlashCommand("ping", ["p"], [
        "   /ping             Test ping"
    ], SlashArgMode.none, 10, (ref AgentApp app, string arg) {
        return AgentStatus.active;
    })).status);
    assert(reg.register(SlashCommand("model", [], [
        "   /model             List available models"
    ], SlashArgMode.optional, 20, (ref AgentApp app, string arg) {
        return arg.empty ? AgentStatus.active : AgentStatus.terminate;
    })).status);
    assert(reg.register(SlashCommand("plan", [], [
        "   /plan <query>      Run the plan pipeline"
    ], SlashArgMode.required, 30, (ref AgentApp app, string arg) {
        return arg == "do-it" ? AgentStatus.terminate : AgentStatus.active;
    })).status);

    // Registration + alias lookup
    assert(reg.execute(app, "/ping") == AgentStatus.active);
    assert(reg.execute(app, "/p") == AgentStatus.active, "alias must dispatch");

    // argModeOf introspection: direct assertion of a registered command's
    // argMode, decoupled from dispatch behavior; unknown names return the enum
    // default (none).
    assert(reg.argModeOf("ping") == SlashArgMode.none);
    assert(reg.argModeOf("model") == SlashArgMode.optional);
    assert(reg.argModeOf("plan") == SlashArgMode.required);
    assert(reg.argModeOf("nope") == SlashArgMode.none);

    // Slash detection
    assert(reg.isSlashCommand("/ping"));
    assert(reg.isSlashCommand("/"));
    assert(!reg.isSlashCommand("ping"));
    assert(!reg.isSlashCommand(""));

    // Unknown command -> active (message emitted via the blocked UiMessenger)
    assert(reg.execute(app, "/nope") == AgentStatus.active);
    assert(reg.execute(app, "/") == AgentStatus.active);

    // ArgMode.none + arg -> unknown (exact-match semantics)
    assert(reg.execute(app, "/ping now") == AgentStatus.active);

    // ArgMode.required + empty arg -> unknown (W1)
    assert(reg.execute(app, "/plan") == AgentStatus.active);
    // ArgMode.required + arg -> handler
    assert(reg.execute(app, "/plan do-it") == AgentStatus.terminate);

    // ArgMode.optional: empty arg dispatches to the handler (e.g. /model lists)
    assert(reg.execute(app, "/model") == AgentStatus.active);
    assert(reg.execute(app, "/model 1") == AgentStatus.terminate);

    // Duplicate registration throws (canonical name and alias)
    assert(!reg.register(SlashCommand("ping", [], [], SlashArgMode.none, 10,
            (ref AgentApp app, string arg) => AgentStatus.active)).status);
    assert(!reg.register(SlashCommand("other", ["p"], [], SlashArgMode.none,
            10, (ref AgentApp app, string arg) => AgentStatus.active)).status);
    // Empty canonical name throws
    assert(!reg.register(SlashCommand("", [], [], SlashArgMode.none, 10,
            (ref AgentApp app, string arg) => AgentStatus.active)).status);
    // Null handler throws (a null delegate would crash at dispatch time)
    assert(!reg.register(SlashCommand("nohandler", [], [], SlashArgMode.none, 10, null)).status);

    // A failed registration (alias collision) must leave the registry
    // unchanged: the name stays unregistered and re-registration succeeds.
    assert(!reg.register(SlashCommand("newcmd", ["ping"], [],
            SlashArgMode.none, 10, (ref AgentApp app, string arg) => AgentStatus.terminate)).status);
    assert(reg.execute(app, "/newcmd") == AgentStatus.active,
            "failed registration must not make the command dispatchable");
    assert(reg.register(SlashCommand("newcmd", [], [], SlashArgMode.none, 10,
            (ref AgentApp app, string arg) => AgentStatus.terminate)).status);
    assert(reg.execute(app, "/newcmd") == AgentStatus.terminate,
            "re-registration after a failed attempt must work");

    // Help text: header + bare-query line + commands sorted by order asc
    auto help = reg.helpText();
    assert(help.startsWith("llmfun agent mode - type a query and press Tab to start.\n"
            ~ " Use /commands for special actions:\n" ~ "\n"
            ~ "   (bare query)       Send a message to the agent\n"));
    assert(help.indexOf("   /ping") < help.indexOf("   /model"));
    assert(help.indexOf("   /model") < help.indexOf("   /plan"));

    // formatHelpLine padding (W4): description at column 22 for usage <= 19,
    // with a two-space gap (column 25) when usage exceeds 19
    auto shortLine = SlashCommandRegistry.formatHelpLine("/ping", "Test");
    assert(shortLine.length >= 22 && shortLine[22 .. $] == "Test");
    auto longLine = SlashCommandRegistry.formatHelpLine("/switch <n|id|title>", "Switch");
    assert(longLine.length >= 25 && longLine[25 .. $] == "Switch");
}

unittest {
    // Help ordering tiebreak (W8): equal order values keep registration order
    import llm.app_config : UserConfig;

    auto app = AgentApp(UserConfig.AgentChatConfig.init);
    SlashCommandRegistry reg;
    assert(reg.register(SlashCommand("b", [], ["   /b"], SlashArgMode.none, 5,
            (ref AgentApp app, string arg) => AgentStatus.active)).status);
    assert(reg.register(SlashCommand("a", [], ["   /a"], SlashArgMode.none, 5,
            (ref AgentApp app, string arg) => AgentStatus.active)).status);

    auto help = reg.helpText();
    assert(help.indexOf("   /b") < help.indexOf("   /a"),
            "equal orders must keep registration order");
}

unittest {
    // Regression test: a startup command registered via addStartupSlashCommand
    // must not make later AgentApp constructions throw. slashCommands_ is a
    // per-instance registry (a fresh empty byName_ per construction), so
    // re-registering the startup list is idempotent across instances — no
    // cross-instance duplicate can exist.
    import llm.app_config : UserConfig;
    import std.algorithm.searching : canFind;

    addStartupSlashCommand(SlashCommand("idem-x", [], [], SlashArgMode.none,
            900, (ref AgentApp app, string arg) => AgentStatus.active));
    auto app1 = AgentApp(UserConfig.AgentChatConfig.init);
    auto app2 = AgentApp(UserConfig.AgentChatConfig.init); // must not throw
}

unittest {
    // isRegistered mirrors execute's tokenization via the shared commandNameOf
    // helper: the dispatcher's /delete parity guard
    // (`query.startsWith("/delete") && !isRegistered(query)`) depends on it.
    import llm.app_config : UserConfig;

    auto app = AgentApp(UserConfig.AgentChatConfig.init); // blocked UiMessenger (W5)
    SlashCommandRegistry reg;
    registerBuiltinCommands(reg);

    // Registered /delete-prefixed forms (all dispatch to the /delete handler)
    assert(reg.isRegistered("/delete"));
    assert(reg.isRegistered("/delete "));
    assert(reg.isRegistered("/delete 5"));
    assert(reg.isRegistered("/delete 5 x"));
    // Other registered commands
    assert(reg.isRegistered("/help"));
    assert(reg.isRegistered("/switch x"));
    assert(reg.isRegistered("/quit"));
    // Unregistered /delete-prefixed typos (the parity-guard trigger)
    assert(!reg.isRegistered("/deletefoo"));
    assert(!reg.isRegistered("/deletefoo 5"));
    assert(!reg.isRegistered("/delete/5"));
    assert(!reg.isRegistered("/delete\t"));
    // Degenerate / non-slash inputs never match
    assert(!reg.isRegistered("/"));
    assert(!reg.isRegistered("delete"));
    assert(!reg.isRegistered(""));
}
