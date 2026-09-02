/// Environment execution context interface for tool dispatch.
/// Defines `EnvironmentContext` for tag-based environment lookup and discovery.
module llm.environment.dispatch;

import std.array : empty;
import std.conv : text;
import std.datetime : Duration, dur;
import std.json : JSONValue, JSONOptions;
import std.sumtype : match;

import my.path : AbsolutePath;

import llm.environment.config : EnvironmentBackend, CommandJoinMode,
    ExecutionType, ContainerConfig, HostConfig, EnvironmentConfig;
import llm.environment.backend : RunnerBackend, ContainerRunner, HostRunner, ExecutionResult;
import llm.tool_call;

mixin RegisterLlmFunctions!();

/// Context interface for environment-based command execution.
/// Implementing classes provide tag-based environment lookup and discovery.
interface EnvironmentContext : Context {
    /// Look up an environment by its tag.
    /// Params:
    ///     tag = Environment tag to look up.
    /// Returns: The matching backend, or `.init` if not found.
    EnvironmentBackend getEnvironment(string tag);

    /// List all loaded execution environments.
    /// Returns: Array of all backends, or empty array if none configured.
    EnvironmentBackend[] listEnvironments();

    /// Get the workarea path for magic word substitution.
    /// Returns: The absolute path to the workarea directory.
    AbsolutePath workArea();

    /// Get the default environment tag from configuration.
    /// Returns: The default tag, or null if not configured.
    string getDefaultEnvironmentTag();

    /// Returns: the max output to read from a command.
    long getMaxOutputBytes();
}

/// Parameters for the listEnvironments tool.
struct ListEnvironmentsParams {
}

/// List available execution environments. Returns a JSON array of sanitized
/// environment entries exposing only safe fields: tag, description, capabilities,
/// isIsolated, and commandJoinMode. Never leaks runtimeCli, options, mounts,
/// workingDir, or other infrastructure configuration.
@Function("List available execution environments. Returns a JSON array of environment entries with tag, description, capabilities, isIsolated, and commandJoinMode.")
ExecuteFuncResult listEnvironments(Context baseCtx, ListEnvironmentsParams params) nothrow {
    mixin(baseContextToSpecific!EnvironmentContext);

    try {
        auto envs = ctx.listEnvironments();

        JSONValue[] entries;
        foreach (env; envs) {
            JSONValue obj;
            obj["tag"] = env.tag;
            obj["description"] = env.description;
            obj["capabilities"] = JSONValue(env.capabilities);
            obj["isIsolated"] = env.isIsolated;
            final switch (env.commandJoinMode) {
            case CommandJoinMode.single:
                obj["commandJoinMode"] = "one command string";
                break;
            case CommandJoinMode.whitespace:
                obj["commandJoinMode"] = "whitespace, no quote";
                break;
            case CommandJoinMode.append:
                obj["commandJoinMode"] = "append";
                break;
            }
            obj["timeoutSeconds"] = env.timeout.total!"seconds";
            entries ~= obj;
        }

        return ExecuteFuncResult(JSONValue(entries)
                .toString(JSONOptions.doNotEscapeSlashes), success: true);
    } catch (Exception e) {
        return ExecuteFuncResult("error: " ~ e.msg, success: false);
    }
}

/// Parameters for the executeCommand tool.
struct ExecuteCommandParams {
    @ParamOptional @ParamDescription(
            "Environment tag to use for execution. If empty, uses the configured default environment.")
    string environmentTag;

    @ParamDescription(
            "Command or parameter for a command to execute. See environment description for details.")
    string[] command;

    @ParamOptional @ParamDescription(
            "Session ID for future session-based execution (currently ignored).")
    string sessionId;
}

/// Execute a command in the specified environment. Resolves the environment tag,
/// instantiates the appropriate RunnerBackend (ContainerRunner or HostRunner),
/// executes the command, and returns the result as JSON.
/// Supports optional environmentTag (defaults to config's defaultEnvironment),
/// empty command validation, and future sessionId placeholder.
@Function("Execute a command in an execution environment. Returns JSON with exitCode, stdout, and stderr. Use environmentTag to select the environment (call listEnvironments() to see available options). If environmentTag is empty, uses the configured default environment.")
ExecuteFuncResult executeCommand(Context baseCtx, ExecuteCommandParams params) nothrow {
    mixin(baseContextToSpecific!EnvironmentContext);

    try {
        if (params.command.empty) {
            return ExecuteFuncResult("error: command must not be empty.", success: false);
        }

        string tag = params.environmentTag;
        if (tag.empty) {
            tag = ctx.getDefaultEnvironmentTag();
            if (tag.empty) {
                return ExecuteFuncResult("error: No environment tag specified and no default environment "
                        ~ "configured. Call listEnvironments() to see available options.",
                        success: false);
            }
        }

        auto env = ctx.getEnvironment(tag);
        if (env.tag.empty) {
            return ExecuteFuncResult(
                    "error: Environment '" ~ tag ~ "' not found. Call listEnvironments() to see available options.",
                    success: false);
        }

        if (params.command.length > 1 && env.commandJoinMode == CommandJoinMode.single) {
            return ExecuteFuncResult(i"error: Environment $(tag) may only be called with parameter `command` as an array of strings with one element"
                    .text, success: false);
        }

        auto runner = env.config.match!((ContainerConfig c) {
            return cast(RunnerBackend) new ContainerRunner(c.runtimeCli, c.image, c.options,
                ctx.workArea(), env.commandJoinMode, timeout: env.timeout,
                maxOutputBytes: ctx.getMaxOutputBytes);
        }, (HostConfig h) {
            return cast(RunnerBackend) new HostRunner(h.options, h.workingDir, h.envVars,
                env.commandJoinMode, ctx.workArea(), timeout: env.timeout,
                maxOutputBytes: ctx.getMaxOutputBytes);
        });

        scope (exit)
            runner.dispose();

        auto result = runner.execute(params.command);

        return ExecuteFuncResult(JSONValue([
            "exitCode": JSONValue(result.exitCode),
            "stdout": JSONValue(result.stdout),
            "stderr": JSONValue(result.stderr)
        ]).toString(JSONOptions.doNotEscapeSlashes), success: true);

    } catch (Exception e) {
        return ExecuteFuncResult("error: " ~ e.msg, success: false);
    }
}

version (unittest) {
    private class MockEnvironmentContext : EnvironmentContext {
        EnvironmentBackend[] envs;
        AbsolutePath workAreaPath;
        string defaultTag;

        override EnvironmentBackend getEnvironment(string tag) {
            foreach (ref env; envs) {
                if (env.tag == tag) {
                    return env;
                }
            }
            return EnvironmentBackend.init;
        }

        override EnvironmentBackend[] listEnvironments() {
            return envs;
        }

        override AbsolutePath workArea() {
            return workAreaPath;
        }

        override string getDefaultEnvironmentTag() {
            return defaultTag;
        }

        override long getMaxOutputBytes() {
            return 424242;
        }
    }
}

/// Test: getEnvironment returns value for known tag.
unittest {
    import llm.environment.config : ContainerConfig, EnvironmentConfig, CommandJoinMode;

    string[][string] emptyOptions;
    auto ctx = new MockEnvironmentContext();
    ctx.envs = [
        EnvironmentBackend(tag: "sandbox", description: "Test sandbox",
                capabilities: ["container"], isIsolated: true, config: EnvironmentConfig(
                    ContainerConfig("docker", "alpine:latest", emptyOptions)),
                commandJoinMode: CommandJoinMode.whitespace, timeout: 60.dur!"seconds")
    ];

    auto result = ctx.getEnvironment("sandbox");

    assert(result.tag == "sandbox");
    assert(result.description == "Test sandbox");
}

/// Test: getEnvironment returns init for unknown tag.
unittest {
    auto ctx = new MockEnvironmentContext();

    auto result = ctx.getEnvironment("nonexistent");

    assert(result.tag == "");
}

/// Test: listEnvironments returns all backends.
unittest {
    import llm.environment.config : ContainerConfig, HostConfig,
        EnvironmentConfig, CommandJoinMode;

    string[][string] emptyOptions;
    string[string] emptyEnvVars;
    string[] emptyPrefixes;
    auto ctx = new MockEnvironmentContext();
    ctx.envs = [
        EnvironmentBackend(tag: "sandbox", description: "Test sandbox",
                capabilities: ["container"], isIsolated: true, config: EnvironmentConfig(
                    ContainerConfig("docker", "alpine:latest", emptyOptions)),
                commandJoinMode: CommandJoinMode.whitespace, timeout: 60.dur!"seconds"),
        EnvironmentBackend(tag: "native", description: "Host execution", capabilities: [
            "host"
        ], isIsolated: false,
        config: EnvironmentConfig(HostConfig(options: emptyOptions, workingDir: "", envVars: emptyEnvVars,
                allowedCommandPrefixes: emptyPrefixes)),
        commandJoinMode: CommandJoinMode.append, timeout: 30.dur!"seconds"),
    ];

    auto result = ctx.listEnvironments();

    assert(result.length == 2);
    assert(result[0].tag == "sandbox");
    assert(result[1].tag == "native");
}

/// Test: listEnvironments returns empty array when no backends.
unittest {
    auto ctx = new MockEnvironmentContext();

    auto result = ctx.listEnvironments();

    assert(result.length == 0);
}

/// Test: cast from Context to EnvironmentContext succeeds.
unittest {
    auto ctx = cast(Context) new MockEnvironmentContext();
    auto specific = cast(EnvironmentContext) ctx;
    assert(specific !is null);
    assert(specific.listEnvironments().length == 0);
}

/// Test: listEnvironments tool returns JSON with only safe fields.
unittest {
    import llm.environment.config : ContainerConfig, HostConfig,
        EnvironmentConfig, CommandJoinMode;
    import std.json : parseJSON;

    string[][string] emptyOptions;
    string[string] emptyEnvVars;
    string[] emptyPrefixes;
    auto ctx = new MockEnvironmentContext();
    ctx.envs = [
        EnvironmentBackend(tag: "sandbox", description: "Test sandbox",
                capabilities: ["container"], isIsolated: true, config: EnvironmentConfig(
                    ContainerConfig("docker", "alpine:latest", emptyOptions)),
                commandJoinMode: CommandJoinMode.whitespace, timeout: 60.dur!"seconds"),
        EnvironmentBackend(tag: "native", description: "Host execution", capabilities: [
            "host"
        ], isIsolated: false,
        config: EnvironmentConfig(HostConfig(options: emptyOptions, workingDir: "", envVars: emptyEnvVars,
                allowedCommandPrefixes: emptyPrefixes)),
        commandJoinMode: CommandJoinMode.append, timeout: 30.dur!"seconds"),
    ];

    auto result = listEnvironments(ctx, ListEnvironmentsParams());

    assert(result.success);
    auto json = parseJSON(result.msg);
    assert(json.array.length == 2);

    // Verify safe fields are present
    assert(json.array[0]["tag"].str == "sandbox");
    assert(json.array[0]["description"].str == "Test sandbox");
    assert(json.array[0]["isIsolated"].boolean == true);
    assert(json.array[0]["commandJoinMode"].str == "whitespace, no quote");
    assert(json.array[0]["capabilities"].array.length == 1);
    assert(json.array[0]["capabilities"].array[0].str == "container");

    assert(json.array[1]["tag"].str == "native");
    assert(json.array[1]["isIsolated"].boolean == false);
    assert(json.array[1]["commandJoinMode"].str == "append");

    // Verify infrastructure fields are NOT present
    assert("runtimeCli" !in json.array[0]);
    assert("options" !in json.array[0]);
    assert("mounts" !in json.array[0]);
    assert("config" !in json.array[0]);
    assert("ttlSeconds" !in json.array[0]);
    // Verify timeoutSeconds IS present (replaces ttlSeconds)
    assert("timeoutSeconds" in json.array[0]);
    assert(json.array[0]["timeoutSeconds"].integer == 60);
}

/// Test: listEnvironments tool returns empty array when no environments.
unittest {
    import std.json : parseJSON;

    auto ctx = new MockEnvironmentContext();

    auto result = listEnvironments(ctx, ListEnvironmentsParams());

    assert(result.success);
    auto json = parseJSON(result.msg);
    assert(json.array.length == 0);
}

/// Test: listEnvironments tool with single entry.
unittest {
    import llm.environment.config : ContainerConfig, EnvironmentConfig, CommandJoinMode;
    import std.json : parseJSON;

    string[][string] emptyOptions;
    auto ctx = new MockEnvironmentContext();
    ctx.envs = [
        EnvironmentBackend(tag: "only", description: "Only environment", capabilities: [
        ], isIsolated: true, config: EnvironmentConfig(ContainerConfig("docker", "alpine:latest",
                emptyOptions)), commandJoinMode: CommandJoinMode.whitespace,
                timeout: 120.dur!"seconds")
    ];

    auto result = listEnvironments(ctx, ListEnvironmentsParams());

    assert(result.success);
    auto json = parseJSON(result.msg);
    assert(json.array.length == 1);
    assert(json.array[0]["tag"].str == "only");
    assert(json.array[0]["description"].str == "Only environment");
    assert(json.array[0]["capabilities"].array.length == 0);
}

// Unit tests for executeCommand()
// ─────────────────────────────────────────────────────────────────────────────

/// Test: executeCommand with empty command returns error.
unittest {
    auto ctx = new MockEnvironmentContext();
    ctx.workAreaPath = AbsolutePath("/workarea");

    auto result = executeCommand(ctx, ExecuteCommandParams(environmentTag: "sandbox",
            command: [], sessionId: ""));

    assert(!result.success);
    assert(result.msg == "error: command must not be empty.");
}

/// Test: executeCommand with unknown tag returns error.
unittest {
    auto ctx = new MockEnvironmentContext();
    ctx.workAreaPath = AbsolutePath("/workarea");

    auto result = executeCommand(ctx, ExecuteCommandParams(environmentTag: "nonexistent",
            command: ["echo"], sessionId: ""));

    assert(!result.success);
    assert(
            result.msg
            == "error: Environment 'nonexistent' not found. Call listEnvironments() to see available options.");
}

/// Test: executeCommand with empty tag uses default environment.
unittest {
    import llm.environment.config : ContainerConfig, EnvironmentConfig, CommandJoinMode;
    import std.algorithm : canFind;
    import std.json : parseJSON;

    string[][string] emptyOptions;
    auto ctx = new MockEnvironmentContext();
    ctx.workAreaPath = AbsolutePath("/workarea");
    ctx.defaultTag = "sandbox";
    ctx.envs = [
        EnvironmentBackend(tag: "sandbox", description: "Test sandbox",
                capabilities: ["container"], isIsolated: true, config: EnvironmentConfig(
                    ContainerConfig("docker", "alpine:latest", emptyOptions)),
                commandJoinMode: CommandJoinMode.whitespace, timeout: 60.dur!"seconds")
    ];

    // Empty environmentTag should use default "sandbox"
    auto result = executeCommand(ctx, ExecuteCommandParams(environmentTag: "",
            command: ["echo", "hello"], sessionId: ""));

    // The call should attempt to run the container (will fail without docker, but should not be a tag error)
    if (!result.success) {
        // If it failed, it should NOT be a "not found" error
        assert(!result.msg.canFind("not found"),
                "Should not be a tag not found error, got: " ~ result.msg);
    }
}

/// Test: executeCommand with no environments returns error.
unittest {
    import std.algorithm : canFind;

    auto ctx = new MockEnvironmentContext();
    ctx.workAreaPath = AbsolutePath("/workarea");

    auto result = executeCommand(ctx, ExecuteCommandParams(environmentTag: "sandbox",
            command: ["echo"], sessionId: ""));

    assert(!result.success);
    assert(result.msg.canFind("not found"));
}

/// Test: executeCommand with empty tag and no default returns clear error.
unittest {
    auto ctx = new MockEnvironmentContext();
    ctx.workAreaPath = AbsolutePath("/workarea");
    // defaultTag is null by default (no default configured)

    auto result = executeCommand(ctx, ExecuteCommandParams(environmentTag: "",
            command: ["echo"], sessionId: ""));

    assert(!result.success);
    assert(result.msg == "error: No environment tag specified and no default environment configured. Call listEnvironments() to see available options.");
}

/// Test: executeCommand with sessionId parameter (ignored).
unittest {
    import llm.environment.config : ContainerConfig, EnvironmentConfig, CommandJoinMode;
    import std.algorithm : canFind;

    string[][string] emptyOptions;
    auto ctx = new MockEnvironmentContext();
    ctx.workAreaPath = AbsolutePath("/workarea");
    ctx.defaultTag = "sandbox";
    ctx.envs = [
        EnvironmentBackend(tag: "sandbox", description: "Test sandbox",
                capabilities: ["container"], isIsolated: true, config: EnvironmentConfig(
                    ContainerConfig("docker", "alpine:latest", emptyOptions)),
                commandJoinMode: CommandJoinMode.whitespace, timeout: 60.dur!"seconds")
    ];

    // sessionId should be accepted but ignored
    auto result = executeCommand(ctx, ExecuteCommandParams(environmentTag: "sandbox",
            command: ["echo"], sessionId: "session-123"));

    // Should not fail due to sessionId
    if (!result.success) {
        assert(!result.msg.canFind("sessionId"), "sessionId should be ignored, got: " ~ result.msg);
    }
}

/// Test: executeCommand with HostConfig creates HostRunner.
unittest {
    import llm.environment.config : HostConfig, EnvironmentConfig, CommandJoinMode;
    import std.algorithm : canFind;

    string[][string] emptyOptions;
    string[string] emptyEnvVars;
    string[] emptyPrefixes;
    auto ctx = new MockEnvironmentContext();
    ctx.workAreaPath = AbsolutePath("/workarea");
    ctx.envs = [
        EnvironmentBackend(tag: "native", description: "Host execution", capabilities: [
            "host"
        ], isIsolated: false,
        config: EnvironmentConfig(HostConfig(options: emptyOptions,
                workingDir: "", envVars: emptyEnvVars, allowedCommandPrefixes: emptyPrefixes)),
        commandJoinMode: CommandJoinMode.whitespace, timeout: 30.dur!"seconds")
    ];

    auto result = executeCommand(ctx, ExecuteCommandParams(environmentTag: "native",
            command: ["echo", "hello"], sessionId: ""));

    // Should attempt host execution (may fail in test env, but not a tag error)
    if (!result.success) {
        assert(!result.msg.canFind("not found"),
                "Should not be a tag not found error, got: " ~ result.msg);
    }
}
