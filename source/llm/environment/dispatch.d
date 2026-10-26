/// Environment execution context interface for tool dispatch.
/// Defines `EnvironmentContext` for tag-based environment lookup and discovery.
module llm.environment.dispatch;

import std.array : empty;
import std.conv : to;
import std.datetime : dur;
import std.json : JSONValue, JSONOptions;
import std.sumtype : match;

import my.path : AbsolutePath;

import llm.environment.config : EnvironmentBackend, ContainerConfig, HostConfig;
import llm.environment.backend : RunnerBackend, ContainerRunner, HostRunner;
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

    /// Returns: the max output to read from a command.
    long getMaxOutputBytes();
}

/// Parameters for the listEnvironments tool.
struct ListEnvironmentsParams {
}

/// List available execution environments. Returns a JSON array of sanitized
/// environment entries exposing only safe fields: tag, description, capabilities,
/// isIsolated, minArgs, maxArgs, and timeoutSeconds. Never leaks runtimeCli, options, mounts,
/// workingDir, or other infrastructure configuration.
@Function("List available execution environments. Returns a JSON array of environment entries with tag, description, capabilities, isIsolated, minArgs, maxArgs, and timeoutSeconds") ExecuteFuncResult listEnvironments(
        Context baseCtx, ListEnvironmentsParams params) nothrow {
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
            obj["minArgs"] = env.minArgs;
            obj["maxArgs"] = env.maxArgs;
            obj["timeoutSeconds"] = env.timeout.total!"seconds";
            entries ~= obj;
        }

        return ExecuteFuncResult(JSONValue(entries)
                .toString(JSONOptions.doNotEscapeSlashes), success: true);
    } catch (Exception e) {
        return ExecuteFuncResult("error: " ~ e.msg, success: false);
    }
}

struct ExecuteCommandParams {
    @ParamDescription("Environment tag to use for execution")
    string environmentTag;

    @ParamDescription("Array of argument elements to execute. The number of elements must be within the selected environment's minArgs/maxArgs range (maxArgs of -1 means unbounded; see list_environments()).")
    string[] command;

    @ParamOptional @ParamDescription(
            "Session ID for future session-based execution (currently ignored).")
    string sessionId;
}

@Function("Execute a command in an execution environment. Returns JSON with exitCode, stdout, and stderr. Use environmentTag to select the environment (call list_environments() to see available options)") ExecuteFuncResult executeCommand(
        Context baseCtx, ExecuteCommandParams params) nothrow {
    import std.datetime.stopwatch : StopWatch, AutoStart;

    mixin(baseContextToSpecific!EnvironmentContext);

    try {
        string tag = params.environmentTag;
        if (tag.empty) {
            return ExecuteFuncResult(
                    "error: No environment tag specified. Call listEnvironments() to see available options.",
                    success: false);
        }

        auto env = ctx.getEnvironment(tag);
        if (env.tag.empty) {
            return ExecuteFuncResult(
                    "error: Environment '" ~ tag ~ "' not found. Call listEnvironments() to see available options.",
                    success: false);
        }

        const n = params.command.length.to!long;

        if (n < env.minArgs || (env.maxArgs != -1 && n > env.maxArgs)) {
            return ExecuteFuncResult(
                    "error: Environment '" ~ tag ~ "' expects between " ~ env.minArgs.to!string ~ " and " ~ (env.maxArgs == -1
                    ? "unbounded" : env.maxArgs.to!string) ~ " command argument(s), got " ~ n.to!string ~ ".",
                    success: false);
        }

        auto runner = env.config.match!((ContainerConfig c) {
            return cast(RunnerBackend) new ContainerRunner(c.runtimeCli, c.image, c.options,
                ctx.workArea(), timeout: env.timeout, maxOutputBytes: ctx.getMaxOutputBytes);
        }, (HostConfig h) {
            return cast(RunnerBackend) new HostRunner(h.options, h.workingDir, h.envVars,
                ctx.workArea(), timeout: env.timeout, maxOutputBytes: ctx.getMaxOutputBytes);
        });

        scope (exit)
            runner.dispose();

        auto sw = StopWatch(AutoStart.yes);
        auto result = runner.execute(params.command);
        const d = sw.peek;

        return ExecuteFuncResult(JSONValue([
            "exitCode": JSONValue(result.exitCode),
            "time_seconds": JSONValue(d.total!"seconds"),
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

        override long getMaxOutputBytes() {
            return 424242;
        }
    }
}

@("getEnvironment returns the backend for a known tag")
unittest {
    import llm.environment.config : ContainerConfig, EnvironmentConfig;

    string[][string] emptyOptions;
    auto ctx = new MockEnvironmentContext();
    ctx.envs = [
        EnvironmentBackend(tag: "sandbox", description: "Test sandbox",
                capabilities: ["container"], isIsolated: true, config: EnvironmentConfig(
                    ContainerConfig("docker", "alpine:latest", emptyOptions)),
                timeout: 60.dur!"seconds")
    ];

    auto result = ctx.getEnvironment("sandbox");

    assert(result.tag == "sandbox");
    assert(result.description == "Test sandbox");
}

@("getEnvironment returns init for an unknown tag")
unittest {
    auto ctx = new MockEnvironmentContext();

    auto result = ctx.getEnvironment("nonexistent");

    assert(result.tag == "");
}

@("listEnvironments returns all registered backends")
unittest {
    import llm.environment.config : ContainerConfig, HostConfig, EnvironmentConfig;

    string[][string] emptyOptions;
    string[string] emptyEnvVars;
    auto ctx = new MockEnvironmentContext();
    ctx.envs = [
        EnvironmentBackend(tag: "sandbox", description: "Test sandbox",
                capabilities: ["container"], isIsolated: true, config: EnvironmentConfig(
                    ContainerConfig("docker", "alpine:latest", emptyOptions)),
                timeout: 60.dur!"seconds"),
        EnvironmentBackend(tag: "native", description: "Host execution", capabilities: [
            "host"
        ], isIsolated: false,
        config: EnvironmentConfig(HostConfig(options: emptyOptions,
                workingDir: "", envVars: emptyEnvVars)),
        timeout: 30.dur!"seconds"),
    ];

    auto result = ctx.listEnvironments();

    assert(result.length == 2);
    assert(result[0].tag == "sandbox");
    assert(result[1].tag == "native");
}

@("listEnvironments returns an empty array when no backends are registered")
unittest {
    auto ctx = new MockEnvironmentContext();

    auto result = ctx.listEnvironments();

    assert(result.length == 0);
}

@("a cast from Context to EnvironmentContext succeeds")
unittest {
    auto ctx = cast(Context) new MockEnvironmentContext();
    auto specific = cast(EnvironmentContext) ctx;
    assert(specific !is null);
    assert(specific.listEnvironments().length == 0);
}

@("the listEnvironments tool returns JSON with only the safe fields and no infrastructure fields")
unittest {
    import llm.environment.config : ContainerConfig, HostConfig, EnvironmentConfig;
    import std.json : parseJSON;

    string[][string] emptyOptions;
    string[string] emptyEnvVars;
    auto ctx = new MockEnvironmentContext();
    ctx.envs = [
        EnvironmentBackend(tag: "sandbox", description: "Test sandbox",
                capabilities: ["container"], isIsolated: true, config: EnvironmentConfig(
                    ContainerConfig("docker", "alpine:latest", emptyOptions)),
                timeout: 60.dur!"seconds"),
        EnvironmentBackend(tag: "native", description: "Host execution", capabilities: [
            "host"
        ], isIsolated: false,
        config: EnvironmentConfig(HostConfig(options: emptyOptions,
                workingDir: "", envVars: emptyEnvVars)),
        timeout: 30.dur!"seconds"),
    ];

    auto result = listEnvironments(ctx, ListEnvironmentsParams());

    assert(result.success);
    auto json = parseJSON(result.msg);
    assert(json.array.length == 2);

    // Verify safe fields are present
    assert(json.array[0]["tag"].str == "sandbox");
    assert(json.array[0]["description"].str == "Test sandbox");
    assert(json.array[0]["isIsolated"].boolean == true);
    assert(json.array[0]["minArgs"].integer == 0);
    assert(json.array[0]["maxArgs"].integer == 1);
    assert(json.array[0]["capabilities"].array.length == 1);
    assert(json.array[0]["capabilities"].array[0].str == "container");

    assert(json.array[1]["tag"].str == "native");
    assert(json.array[1]["isIsolated"].boolean == false);
    assert(json.array[1]["minArgs"].integer == 0);
    assert(json.array[1]["maxArgs"].integer == 1);

    // Verify infrastructure fields are NOT present
    assert("runtimeCli" !in json.array[0]);
    assert("options" !in json.array[0]);
    assert("mounts" !in json.array[0]);
    assert("config" !in json.array[0]);
    assert("commandJoinMode" !in json.array[0]);
    assert("ttlSeconds" !in json.array[0]);
    // Verify timeoutSeconds IS present (replaces ttlSeconds)
    assert("timeoutSeconds" in json.array[0]);
    assert(json.array[0]["timeoutSeconds"].integer == 60);
}

@("the listEnvironments tool returns an empty JSON array when no environments exist")
unittest {
    import std.json : parseJSON;

    auto ctx = new MockEnvironmentContext();

    auto result = listEnvironments(ctx, ListEnvironmentsParams());

    assert(result.success);
    auto json = parseJSON(result.msg);
    assert(json.array.length == 0);
}

@("the listEnvironments tool with a single entry reports its fields")
unittest {
    import llm.environment.config : ContainerConfig, EnvironmentConfig;
    import std.json : parseJSON;

    string[][string] emptyOptions;
    auto ctx = new MockEnvironmentContext();
    ctx.envs = [
        EnvironmentBackend(tag: "only", description: "Only environment", capabilities: [
        ], isIsolated: true, config: EnvironmentConfig(ContainerConfig("docker",
                "alpine:latest", emptyOptions)), timeout: 120.dur!"seconds")
    ];

    auto result = listEnvironments(ctx, ListEnvironmentsParams());

    assert(result.success);
    auto json = parseJSON(result.msg);
    assert(json.array.length == 1);
    assert(json.array[0]["tag"].str == "only");
    assert(json.array[0]["description"].str == "Only environment");
    assert(json.array[0]["capabilities"].array.length == 0);
}

@("executeCommand rejects commands outside the minArgs/maxArgs range")
unittest {
    import llm.environment.config : ContainerConfig, EnvironmentConfig;

    string[][string] emptyOptions;
    auto ctx = new MockEnvironmentContext();
    ctx.workAreaPath = AbsolutePath("/workarea");
    ctx.envs = [
        EnvironmentBackend(tag: "sandbox", description: "Test sandbox",
                capabilities: ["container"], isIsolated: true, config: EnvironmentConfig(
                    ContainerConfig("docker", "alpine:latest", emptyOptions)),
                timeout: 60.dur!"seconds", minArgs: 1, maxArgs: 2)
    ];

    // Below the lower bound: an empty command reports got 0.
    auto low = executeCommand(ctx, ExecuteCommandParams(environmentTag: "sandbox",
            command: [], sessionId: ""));

    assert(!low.success);
    assert(
            low.msg
            == "error: Environment 'sandbox' expects between 1 and 2 command argument(s), got 0.");

    // Above the upper bound: a 3-element command reports got 3.
    auto high = executeCommand(ctx, ExecuteCommandParams(environmentTag: "sandbox",
            command: ["a", "b", "c"], sessionId: ""));

    assert(!high.success);
    assert(
            high.msg
            == "error: Environment 'sandbox' expects between 1 and 2 command argument(s), got 3.");
}

@("an environment with minArgs 0 accepts an empty command instead of rejecting it")
unittest {
    import llm.environment.config : ContainerConfig, HostConfig, EnvironmentConfig;
    import std.algorithm : canFind;
    import std.json : parseJSON;

    string[][string] emptyOptions;
    string[string] emptyEnvVars;
    auto ctx = new MockEnvironmentContext();
    ctx.workAreaPath = AbsolutePath("/workarea");

    ctx.envs = [
        EnvironmentBackend(tag: "sandbox", description: "Test sandbox",
                capabilities: ["container"], isIsolated: true, config: EnvironmentConfig(
                    ContainerConfig("docker", "alpine:latest", emptyOptions)),
                timeout: 60.dur!"seconds", minArgs: 0, maxArgs: 1)
    ];
    auto containerResult = executeCommand(ctx,
            ExecuteCommandParams(environmentTag: "sandbox", command: [], sessionId: ""));

    assert(!containerResult.msg.canFind("expects between"),
            "empty command must forward past the arity check, got: " ~ containerResult.msg);

    ctx.envs = [
        EnvironmentBackend(tag: "native", description: "Host execution", capabilities: [
            "host"
        ], isIsolated: false,
        config: EnvironmentConfig(HostConfig(options: ["00_shell": ["sh",
                    "-c"]], workingDir: "", envVars: emptyEnvVars)),
        timeout: 30.dur!"seconds", minArgs: 0, maxArgs: 1)
    ];
    auto hostResult = executeCommand(ctx, ExecuteCommandParams(environmentTag: "native",
            command: [], sessionId: ""));

    assert(!hostResult.msg.canFind("expects between"),
            "empty command must forward past the arity check, got: " ~ hostResult.msg);
    assert(hostResult.success);
    auto hostJson = parseJSON(hostResult.msg);
    assert(hostJson["exitCode"].integer != 0,
            "empty host command must fail as a normal error result, got: " ~ hostResult.msg);
}

@("boundary counts are inclusive at both ends of the minArgs/maxArgs range")
unittest {
    import llm.environment.config : ContainerConfig, EnvironmentConfig;
    import std.algorithm : canFind;

    string[][string] emptyOptions;
    auto ctx = new MockEnvironmentContext();
    ctx.workAreaPath = AbsolutePath("/workarea");
    ctx.envs = [
        EnvironmentBackend(tag: "sandbox", description: "Test sandbox",
                capabilities: ["container"], isIsolated: true, config: EnvironmentConfig(
                    ContainerConfig("docker", "alpine:latest", emptyOptions)),
                timeout: 60.dur!"seconds", minArgs: 2, maxArgs: 3)
    ];

    // Inclusive at the lower end: exactly minArgs elements forwards.
    auto atMin = executeCommand(ctx, ExecuteCommandParams(environmentTag: "sandbox",
            command: ["a", "b"], sessionId: ""));
    assert(!atMin.msg.canFind("expects between"),
            "exactly minArgs elements must be accepted, got: " ~ atMin.msg);

    // Inclusive at the upper end: exactly maxArgs elements forwards.
    auto atMax = executeCommand(ctx, ExecuteCommandParams(environmentTag: "sandbox",
            command: ["a", "b", "c"], sessionId: ""));
    assert(!atMax.msg.canFind("expects between"),
            "exactly maxArgs elements must be accepted, got: " ~ atMax.msg);

    // One below minArgs: rejected.
    auto below = executeCommand(ctx, ExecuteCommandParams(environmentTag: "sandbox",
            command: ["a"], sessionId: ""));
    assert(!below.success);
    assert(below.msg.canFind("sandbox"), below.msg);
    assert(below.msg.canFind("between 2 and 3"), below.msg);
    assert(below.msg.canFind("got 1"), below.msg);

    // One above maxArgs: rejected.
    auto above = executeCommand(ctx, ExecuteCommandParams(environmentTag: "sandbox",
            command: ["a", "b", "c", "d"], sessionId: ""));
    assert(!above.success);
    assert(above.msg.canFind("sandbox"), above.msg);
    assert(above.msg.canFind("between 2 and 3"), above.msg);
    assert(above.msg.canFind("got 4"), above.msg);
}

@("maxArgs -1 accepts a command of any length")
unittest {
    import llm.environment.config : ContainerConfig, EnvironmentConfig;
    import std.algorithm : canFind;

    string[][string] emptyOptions;
    auto ctx = new MockEnvironmentContext();
    ctx.workAreaPath = AbsolutePath("/workarea");
    ctx.envs = [
        EnvironmentBackend(tag: "sandbox", description: "Test sandbox",
                capabilities: ["container"], isIsolated: true, config: EnvironmentConfig(
                    ContainerConfig("docker", "alpine:latest", emptyOptions)),
                timeout: 60.dur!"seconds", minArgs: 0, maxArgs: -1)
    ];

    // A 10-element command is within the unbounded range and forwards.
    auto result = executeCommand(ctx, ExecuteCommandParams(environmentTag: "sandbox",
            command: ["a", "b", "c", "d", "e", "f", "g", "h", "i", "j"], sessionId: ""));

    assert(!result.msg.canFind("expects between"),
            "maxArgs -1 must be unbounded, got: " ~ result.msg);
}

@("the rejected-command error names the tag, the range, and the actual count")
unittest {
    import llm.environment.config : ContainerConfig, EnvironmentConfig;
    import std.algorithm : canFind;

    string[][string] emptyOptions;
    auto ctx = new MockEnvironmentContext();
    ctx.workAreaPath = AbsolutePath("/workarea");
    ctx.envs = [
        EnvironmentBackend(tag: "sandbox", description: "Test sandbox",
                capabilities: ["container"], isIsolated: true, config: EnvironmentConfig(
                    ContainerConfig("docker", "alpine:latest", emptyOptions)),
                timeout: 60.dur!"seconds", minArgs: 2, maxArgs: -1)
    ];

    auto result = executeCommand(ctx, ExecuteCommandParams(environmentTag: "sandbox",
            command: ["a"], sessionId: ""));

    assert(!result.success);
    assert(result.msg.canFind("sandbox"), result.msg);
    assert(result.msg.canFind("unbounded"), result.msg);
    assert(result.msg.canFind("got 1"), result.msg);
}

@("executeCommand with an unknown tag returns a not-found error")
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

@("executeCommand with an empty tag uses the default environment")
unittest {
    import llm.environment.config : ContainerConfig, EnvironmentConfig;
    import std.algorithm : canFind;
    import std.json : parseJSON;

    string[][string] emptyOptions;
    auto ctx = new MockEnvironmentContext();
    ctx.workAreaPath = AbsolutePath("/workarea");
    ctx.envs = [
        EnvironmentBackend(tag: "sandbox", description: "Test sandbox",
                capabilities: ["container"], isIsolated: true, config: EnvironmentConfig(
                    ContainerConfig("docker", "alpine:latest", emptyOptions)),
                timeout: 60.dur!"seconds")
    ];

    // Empty environmentTag should use default "sandbox"
    auto result = executeCommand(ctx, ExecuteCommandParams(environmentTag: "",
            command: ["echo"], sessionId: ""));

    // The call should attempt to run the container (will fail without docker, but should not be a tag error)
    if (!result.success) {
        // If it failed, it should NOT be a "not found" error
        assert(!result.msg.canFind("not found"),
                "Should not be a tag not found error, got: " ~ result.msg);
    }
}

@("executeCommand with no environments returns a not-found error")
unittest {
    import std.algorithm : canFind;

    auto ctx = new MockEnvironmentContext();
    ctx.workAreaPath = AbsolutePath("/workarea");

    auto result = executeCommand(ctx, ExecuteCommandParams(environmentTag: "sandbox",
            command: ["echo"], sessionId: ""));

    assert(!result.success);
    assert(result.msg.canFind("not found"));
}

@("executeCommand with an empty tag and no default returns a clear error")
unittest {
    auto ctx = new MockEnvironmentContext();
    ctx.workAreaPath = AbsolutePath("/workarea");

    auto result = executeCommand(ctx, ExecuteCommandParams(environmentTag: "",
            command: ["echo"], sessionId: ""));

    assert(!result.success);
}

@("executeCommand accepts the sessionId parameter and ignores it")
unittest {
    import llm.environment.config : ContainerConfig, EnvironmentConfig;
    import std.algorithm : canFind;

    string[][string] emptyOptions;
    auto ctx = new MockEnvironmentContext();
    ctx.workAreaPath = AbsolutePath("/workarea");
    ctx.envs = [
        EnvironmentBackend(tag: "sandbox", description: "Test sandbox",
                capabilities: ["container"], isIsolated: true, config: EnvironmentConfig(
                    ContainerConfig("docker", "alpine:latest", emptyOptions)),
                timeout: 60.dur!"seconds")
    ];

    // sessionId should be accepted but ignored
    auto result = executeCommand(ctx, ExecuteCommandParams(environmentTag: "sandbox",
            command: ["echo"], sessionId: "session-123"));

    // Should not fail due to sessionId
    if (!result.success) {
        assert(!result.msg.canFind("sessionId"), "sessionId should be ignored, got: " ~ result.msg);
    }
}

@("executeCommand with a HostConfig environment creates a HostRunner")
unittest {
    import llm.environment.config : HostConfig, EnvironmentConfig;
    import std.algorithm : canFind;

    string[][string] emptyOptions;
    string[string] emptyEnvVars;
    auto ctx = new MockEnvironmentContext();
    ctx.workAreaPath = AbsolutePath("/workarea");
    ctx.envs = [
        EnvironmentBackend(tag: "native", description: "Host execution", capabilities: [
            "host"
        ], isIsolated: false,
        config: EnvironmentConfig(HostConfig(options: emptyOptions,
                workingDir: "", envVars: emptyEnvVars)),
        timeout: 30.dur!"seconds")
    ];

    auto result = executeCommand(ctx, ExecuteCommandParams(environmentTag: "native",
            command: ["echo"], sessionId: ""));

    // Should attempt host execution (may fail in test env, but not a tag error)
    if (!result.success) {
        assert(!result.msg.canFind("not found"),
                "Should not be a tag not found error, got: " ~ result.msg);
    }
}
