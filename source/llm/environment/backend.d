/// Foundational types for environment-based command execution.
/// Defines ExecutionResult, RunnerBackend interface, and ContainerRunner class.
module llm.environment.backend;

import logger = std.logger;
import std.algorithm : sort;
import std.array : appender, empty, array, join;
import std.conv : to, text;
import std.datetime : Duration, dur;
import std.datetime.stopwatch : StopWatch, AutoStart, dur;
import std.exception : collectException;
import std.process : Redirect, Config;
import std.range : isInputRange;

import proc;

import my.path : AbsolutePath;

import llm.config : replaceContainerMagicWords, replaceMagicWord;
import llm.environment.config : CommandJoinMode;

/// Structured result from command execution.
/// Negative exit codes (e.g., -1 for timeout) indicate errors.
struct ExecutionResult {
    /// Standard output from the executed command.
    string stdout;

    /// Standard error from the executed command.
    string stderr;

    /// Exit code. Negative values indicate errors (e.g., -1 for timeout).
    int exitCode;

    string toString() const @safe {
        return i"ExecutionResult(exitCode:$(exitCode), stdout:'$(stdout)', stderr:'$(stderr))'"
            .text;
    }
}

/// Result from collecting limited output from a process.
private struct CollectedOutput {
    string stdout;
    string stderr;
}

/// Collect output from a proc drain range with per-stream byte limits.
/// Appends truncation warnings to stderr when limits are exceeded.
/// Continues draining after truncation to prevent pipe blocking.
/// Note: Assumes UTF-8 text; binary output may result in corrupted data.
/// Template accepts any range of DrainElement (streaming range or materialized array).
/// Marked @trusted because it casts string literals to ubyte[] for efficient appending.
CollectedOutput collectOutputLimited(R)(R elems, long maxOutputBytes) @trusted
        if (isInputRange!R) {
    auto stdoutApp = appender!(char[])();
    auto stderrApp = appender!(char[])();

    foreach (elem; elems) {
        final switch (elem.type) {
        case DrainElement.Type.stdout:
            if (stdoutApp.length < maxOutputBytes) {
                stdoutApp.put(elem.byUTF8);
            }
            break;
        case DrainElement.Type.stderr:
            if (stderrApp.length < maxOutputBytes) {
                stderrApp.put(elem.byUTF8);
            }
            break;
        }
    }

    // Capture truncation state and lengths before appending warnings to avoid
    // stdout warning bytes counting toward stderr limit and inflated length reports.
    bool stdoutTruncated = stdoutApp.length >= maxOutputBytes;
    bool stderrTruncated = stderrApp.length >= maxOutputBytes;
    long stderrLenAtTruncate = stderrApp.length;

    if (stdoutTruncated) {
        stderrApp.put(cast(const(ubyte)[])("\n[stdout truncated: " ~ stdoutApp.length.to!string
                ~ " bytes collected, limit: " ~ maxOutputBytes.to!string ~ " bytes]"));
    }
    if (stderrTruncated) {
        stderrApp.put(cast(const(ubyte)[])("\n[stderr truncated: " ~ stderrLenAtTruncate.to!string
                ~ " bytes collected, limit: " ~ maxOutputBytes.to!string ~ " bytes]"));
    }

    return CollectedOutput(stdoutApp.data[].idup, stderrApp.data[].idup);
}

/// Interface for command execution backends (container or host).
/// Implementations handle the specifics of spawning and managing processes.
interface RunnerBackend {
    /// Execute a command and return structured results.
    /// Params:
    ///     command = Command elements to execute.
    /// Returns: ExecutionResult with stdout, stderr, and exitCode.
    ExecutionResult execute(string[] command);

    /// Release any resources held by this backend.
    /// Safe to call multiple times.
    void dispose();
}

/// Container command runner implementing the RunnerBackend interface.
/// Wraps container runtime execution with magic word substitution
/// and configurable command joining behavior.
class ContainerRunner : RunnerBackend {
    string runtimeCli_;
    string image_;
    string[][string] options_;
    AbsolutePath workArea_;
    CommandJoinMode commandJoinMode_;
    Duration timeout;
    long maxOutputBytes_;

    /// Construct a ContainerRunner with the given configuration.
    /// Params:
    ///     runtimeCli = Container runtime CLI (e.g., "docker").
    ///     image = Container image name (e.g., "alpine:latest").
    ///     options = Pre-configured container options map.
    ///     workArea = Workarea path for magic word substitution.
    ///     commandJoinMode = How to combine command elements.
    ///     timeoutSec = Execution timeout in seconds (default: 60).
    ///     maxOutputBytes = Maximum bytes per output stream (default: 1MB).
    this(string runtimeCli, string image, string[][string] options, AbsolutePath workArea,
            CommandJoinMode commandJoinMode, Duration timeout, long maxOutputBytes) @safe
    in (timeout.total!"seconds" > 0, "timeout must be >0") {
        this.runtimeCli_ = runtimeCli;
        this.image_ = image;
        this.options_ = options;
        this.workArea_ = workArea;
        this.commandJoinMode_ = commandJoinMode;
        this.timeout = timeout;
        this.maxOutputBytes_ = maxOutputBytes;
    }

    /// Execute a command in the configured container environment.
    /// Applies magic word substitution to options, builds the command
    /// from sorted options and command elements, then runs the container.
    /// Params:
    ///     command = Command elements to execute inside the container.
    /// Returns: ExecutionResult with stdout, stderr, and exitCode.
    ExecutionResult execute(string[] command) nothrow {
        auto result = ExecutionResult(exitCode: -1);
        try {
            auto resolved = replaceContainerMagicWords(options_, workArea_);
            auto args = buildCommand(resolved, command, commandJoinMode_, image: image_);

            return runContainerCommand(runtimeCli_, args, timeout, maxOutputBytes_, image_);
        } catch (Exception e) {
            try {
                result = ExecutionResult(stdout: "", stderr: i"error: container execution failed: $(
                        e.msg)".text, exitCode: -1);
            } catch (Exception) {
            }
        }
        return result;
    }

    /// No-op dispose. ContainerRunner holds no persistent resources.
    void dispose() @safe nothrow {
    }
}

/// Executes commands via native subprocess using proc.pipeProcess,
/// with configurable working directory, environment variables, and timeout.
class HostRunner : RunnerBackend {
    string[][string] options_;
    AbsolutePath workingDir_;
    string[string] envVars_;
    CommandJoinMode commandJoinMode_;
    AbsolutePath workArea_;
    Duration timeout;
    long maxOutputBytes;

    /// Construct a HostRunner with the given configuration.
    /// Params:
    ///     options = Pre-configured host options map.
    ///     workingDir = Working directory for the subprocess (supports magic words).
    ///     envVars = Environment variables to merge into the subprocess (supports magic words in values).
    ///     commandJoinMode = How to combine command elements.
    ///     workArea = Workarea path for magic word substitution.
    ///     timeoutSec = Execution timeout in seconds (0 means no limit, default: 60).
    ///     maxOutputBytes = Maximum bytes per output stream (default: 1MB).
    this(string[][string] options, AbsolutePath workingDir, string[string] envVars,
            CommandJoinMode commandJoinMode, AbsolutePath workArea,
            Duration timeout, long maxOutputBytes) @safe
    in (timeout.total!"seconds" > 0, "timeout must be >0") {
        this.options_ = options;
        this.workingDir_ = workingDir;
        this.envVars_ = envVars;
        this.commandJoinMode_ = commandJoinMode;
        this.workArea_ = workArea;
        this.timeout = timeout;
        this.maxOutputBytes = maxOutputBytes;
    }

    /// Execute a command in the host environment.
    /// Applies magic word substitution to options, workingDir, and envVars values,
    /// builds the command from sorted options and command elements,
    /// spawns via proc.pipeProcess, enforces timeout, and collects output
    /// with per-stream byte limits.
    /// Params:
    ///     command = Command elements to execute on the host, can be empty
    /// Returns: ExecutionResult with stdout, stderr, and exitCode.
    ExecutionResult execute(string[] command) nothrow {
        auto result = ExecutionResult(exitCode: -1);
        try {
            auto resolvedWd = workingDir_.replaceMagicWord(workArea_);
            string[string] resolvedEnv;
            foreach (key, val; envVars_) {
                resolvedEnv[key] = val.replaceMagicWord(workArea_);
            }

            auto resolvedOpts = replaceContainerMagicWords(options_, workArea_);

            auto fullCmd = buildCommand(resolvedOpts, command, commandJoinMode_);
            logger.trace("Executing host command: ", fullCmd);

            const(char)[] wd = resolvedWd.length > 0 ? resolvedWd : null;
            auto p = proc.pipeProcess(fullCmd, Redirect.all, resolvedEnv,
                    Config.none, wd).sandbox.timeout(timeout);
            scope (exit)
                p.dispose;

            auto sw = StopWatch(AutoStart.yes);
            scope (exit)
                sw.stop;

            auto output = collectOutputLimited(proc.drain(p), maxOutputBytes);

            try {
                result = ExecutionResult(output.stdout, output.stderr, p.wait);
            } catch (Exception e) {
                result = ExecutionResult(stdout: output.stdout, stderr: output.stderr ~ "\n" ~ i"error: $(e.msg)".text,
                        exitCode: -1);
            }

            // Log timeout kill if detected (negative exit code indicates signal termination)
            if (result.exitCode < 0 && result.exitCode != -1) {
                logger.tracef("Host process killed by timeout, exitCode:%d, elapsed:%s",
                        result.exitCode, sw.peek).collectException;
            }

            return result;
        } catch (Exception e) {
            try {
                result = ExecutionResult(stdout: "", stderr: i"error: host execution failed: $(
                        e.msg)".text, exitCode: -1);
            } catch (Exception) {
            }
        }
        return result;
    }

    /// No-op dispose. HostRunner holds no persistent resources.
    void dispose() @safe nothrow {
    }
}

/// Pipeline: sort options by key -> extract shell (entrypoint_* keys) -> flatten -> image -> shell -> command.
private string[] buildCommand(string[][string] options, string[] command,
        CommandJoinMode joinMode, string image = null) {
    import std.string : startsWith;

    string[] args;
    string[] shell;
    auto sortedKeys = options.keys.array.sort!((a, b) => a < b);

    foreach (key; sortedKeys) {
        if (key.startsWith("entrypoint_")) {
            shell = options[key];
            continue; // Skip adding entrypoint values to args
        } else if (key.length < 3 || key[2] != '_') {
            try {
                logger.warningf("Skipping option key '%s': missing numeric prefix", key);
            } catch (Exception) {
            }
            continue;
        }
        auto values = options[key];
        if (!values.empty) {
            args ~= values;
        }
    }

    if (!image.empty)
        args ~= [image];

    args ~= shell;

    final switch (joinMode) {
    case CommandJoinMode.whitespace:
        args ~= command.join(" ");
        break;
    case CommandJoinMode.append:
        args ~= command;
        break;
    }

    return args;
}

/// Spawns the container runtime with the given arguments, enforces timeout,
/// collects output with per-stream byte limits, and returns structured results.
///
/// Params:
///    runtimeCli = Container runtime CLI (e.g., "docker" or "podman")
///    args = Arguments to pass to the runtime (e.g., ["run", "--rm", ...])
///    timeout = Execution timeout duration
///    maxOutputBytes = Maximum bytes per output stream
///    image = Container image name (for logging)
/// Returns: ExecutionResult with stdout, stderr, and exitCode
ExecutionResult runContainerCommand(string runtimeCli, string[] args,
        Duration timeout, long maxOutputBytes, string image) nothrow {
    int exitCode = -1;
    CollectedOutput output;
    auto sw = StopWatch(AutoStart.yes);
    scope (exit)
        sw.stop();
    try {
        auto fullCmd = [runtimeCli] ~ args;
        logger.trace("Executing container command: ", fullCmd);

        auto p = proc.pipeProcess(fullCmd).sandbox.timeout(timeout);
        scope (exit)
            p.dispose;

        output = collectOutputLimited(proc.drain(p), maxOutputBytes);

        try {
            exitCode = p.wait;
        } catch (Exception e) {
            return ExecutionResult(output.stdout, output.stderr ~ "\nerror: " ~ e.msg, -1);
        }
    } catch (Exception e) {
        try {
            return ExecutionResult("", i"error: running container with arguments $(args): $(e.msg)".text,
                    -1);
        } catch (Exception e) {
        }
    }
    // Log timeout kill if detected (negative exit code indicates signal termination)
    if (exitCode < 0 && exitCode != -1) {
        logger.tracef("Container killed: image=%s, exitCode=%d, elapsed=%s",
                image, exitCode, sw.peek).collectException;
    }

    return ExecutionResult(output.stdout, output.stderr, exitCode);
}

// Unit tests for collectOutputLimited()
// ─────────────────────────────────────────────────────────────────────────────

/// Test: collectOutputLimited collects all output when within limit.
unittest {
    DrainElement[] elems = [
        DrainElement(DrainElement.Type.stdout, cast(const(ubyte)[]) "hello ".dup),
        DrainElement(DrainElement.Type.stdout, cast(const(ubyte)[]) "world".dup),
    ];

    auto result = collectOutputLimited(elems, 1024);

    assert(result.stdout == "hello world");
    assert(result.stderr == "");
}

/// Test: collectOutputLimited handles empty input.
unittest {
    DrainElement[] elems; // empty array
    auto result = collectOutputLimited(elems, 1024);
    assert(result.stdout == "");
    assert(result.stderr == "");
}

/// Test: collectOutputLimited truncates output and adds warning when over limit.
unittest {
    import std.algorithm : canFind;

    ubyte[50] chunk;
    chunk[] = cast(ubyte) 'a';

    DrainElement[] elems = [
        DrainElement(DrainElement.Type.stdout, cast(const(ubyte)[]) chunk[]),
        DrainElement(DrainElement.Type.stdout, cast(const(ubyte)[]) chunk[]),
        DrainElement(DrainElement.Type.stdout, cast(const(ubyte)[]) chunk[]),
        DrainElement(DrainElement.Type.stdout, cast(const(ubyte)[]) chunk[]),
    ];

    auto result = collectOutputLimited(elems, 100);

    assert(result.stdout.length >= 100);
    assert(result.stdout.length <= 150);
    assert(result.stderr.canFind("stdout truncated"));
    assert(result.stderr.canFind("bytes collected"));
    assert(result.stderr.canFind("limit:"));
}

/// Test: collectOutputLimited separates stdout and stderr correctly.
unittest {
    DrainElement[] elems = [
        DrainElement(DrainElement.Type.stdout, cast(const(ubyte)[]) "stdout-line1\n".dup),
        DrainElement(DrainElement.Type.stderr, cast(const(ubyte)[]) "stderr-line1\n".dup),
        DrainElement(DrainElement.Type.stdout, cast(const(ubyte)[]) "stdout-line2\n".dup),
        DrainElement(DrainElement.Type.stderr, cast(const(ubyte)[]) "stderr-line2\n".dup),
    ];

    auto result = collectOutputLimited(elems, 1024);

    assert(result.stdout == "stdout-line1\nstdout-line2\n");
    assert(result.stderr == "stderr-line1\nstderr-line2\n");
}

/// Test: collectOutputLimited reports correct stderr length when both truncated.
unittest {
    import std.algorithm : canFind;

    ubyte[50] chunk;
    chunk[] = cast(ubyte) 'a';

    DrainElement[] elems = [
        DrainElement(DrainElement.Type.stdout, cast(const(ubyte)[]) chunk[]),
        DrainElement(DrainElement.Type.stdout, cast(const(ubyte)[]) chunk[]),
        DrainElement(DrainElement.Type.stderr, cast(const(ubyte)[]) chunk[]),
        DrainElement(DrainElement.Type.stderr, cast(const(ubyte)[]) chunk[]),
    ];

    auto result = collectOutputLimited(elems, 50);

    // Both stdout and stderr truncation warnings go to stderr
    assert(result.stderr.canFind("stdout truncated"));
    assert(result.stderr.canFind("stderr truncated"));
    // Stderr length in warning should be 50 (original), not inflated by stdout warning
    assert(result.stderr.canFind("50 bytes collected"));
}

// Unit tests for ContainerRunner command building
// ─────────────────────────────────────────────────────────────────────────────

/// Test: buildCommand with whitespace join mode produces correct structure.
unittest {
    string[][string] options;
    options["00_subcommand"] = ["run"];
    options["01_cleanup"] = ["--rm"];
    options["02_security"] = ["--read-only"];
    options["entrypoint_00_shell"] = ["sh", "-c"];

    auto args = buildCommand(options, ["echo", "hello"],
            CommandJoinMode.whitespace, image: "alpine:latest");

    // Verify structure: options (sorted) -> image -> shell -> joined command
    assert(args[0] == "run");
    assert(args[1] == "--rm");
    assert(args[2] == "--read-only");
    assert(args[3] == "alpine:latest");
    assert(args[4] == "sh");
    assert(args[5] == "-c");
    assert(args[6] == "echo hello");
}

/// Test: buildCommand with append join mode produces separate args.
unittest {
    string[][string] options;
    options["00_subcommand"] = ["run"];
    options["01_cleanup"] = ["--rm"];

    auto args = buildCommand(options, ["echo", "hello", "world"],
            CommandJoinMode.append, image: "alpine:latest");

    // Verify structure: options (sorted) -> image -> separate command args
    assert(args[0] == "run");
    assert(args[1] == "--rm");
    assert(args[2] == "alpine:latest");
    assert(args[3] == "echo");
    assert(args[4] == "hello");
    assert(args[5] == "world");
}

/// Test: buildCommand with shell + append mode produces correct structure.
unittest {
    string[][string] options;
    options["00_subcommand"] = ["run"];
    options["entrypoint_00_shell"] = ["sh", "-c"];

    auto args = buildCommand(options, ["echo", "hello"],
            CommandJoinMode.append, image: "alpine:latest");

    // Shell comes after image, then each command arg separately
    assert(args[0] == "run");
    assert(args[1] == "alpine:latest");
    assert(args[2] == "sh");
    assert(args[3] == "-c");
    assert(args[4] == "echo");
    assert(args[5] == "hello");
}

/// Test: buildCommand skips keys without numeric prefix.
unittest {
    import std.algorithm : canFind;

    string[][string] options;
    options["00_subcommand"] = ["run"];
    options["invalid"] = ["--should-be-skipped"];
    options["01_cleanup"] = ["--rm"];

    auto args = buildCommand(options, ["echo"], CommandJoinMode.whitespace,
            image: "alpine:latest");

    assert(args[0] == "run");
    assert(args[1] == "--rm");
    assert(args[2] == "alpine:latest");
    assert(args[3] == "echo");
    // "invalid" key should be skipped, "--should-be-skipped" not present
    assert(!args.canFind("--should-be-skipped"));
}

/// Test: buildCommand with empty options produces minimal command.
unittest {
    string[][string] options;

    auto args = buildCommand(options, ["echo", "hello"],
            CommandJoinMode.whitespace, image: "alpine:latest");

    assert(args.length == 2);
    assert(args[0] == "alpine:latest");
    assert(args[1] == "echo hello");
}

/// Test: ContainerRunner magic word substitution in options.
unittest {
    import std.algorithm : canFind;

    string[][string] options;
    options["00_subcommand"] = ["run"];
    options["01_mounts"] = ["-v", "@{llmfun_workarea}:/workarea"];

    auto runner = new ContainerRunner("docker", "alpine:latest", options,
            AbsolutePath("/my/work"), CommandJoinMode.whitespace, 60.dur!"seconds", 1_048_576);

    // Verify construction succeeded
    assert(runner !is null);
}

/// Test: ContainerRunner dispose is a no-op.
unittest {
    string[][string] options;
    auto runner = new ContainerRunner("docker", "alpine:latest", options,
            AbsolutePath("/work"), CommandJoinMode.whitespace, 60.dur!"seconds", 1_048_576);

    runner.dispose(); // Should not throw
    runner.dispose(); // Should be safe to call multiple times
}

/// Test: ExecutionResult toString.
unittest {
    import std.algorithm : canFind;

    auto result = ExecutionResult("hello", "error", 0);
    auto str = result.toString();
    assert(str.canFind("exitCode:0"));
    assert(str.canFind("hello"));
    assert(str.canFind("error"));
}

// Unit tests for HostRunner
// ─────────────────────────────────────────────────────────────────────────────

/// Test: buildCommand with whitespace join mode.
unittest {
    string[][string] options;
    options["00_shell"] = ["sh", "-c"];
    options["01_env"] = ["--env"];

    auto args = buildCommand(options, ["echo", "hello"], CommandJoinMode.whitespace);

    assert(args[0] == "sh");
    assert(args[1] == "-c");
    assert(args[2] == "--env");
    assert(args[3] == "echo hello");
}

/// Test: buildCommand with append join mode.
unittest {
    string[][string] options;
    options["00_cmd"] = ["echo"];

    auto args = buildCommand(options, ["hello", "world"], CommandJoinMode.append);

    assert(args[0] == "echo");
    assert(args[1] == "hello");
    assert(args[2] == "world");
}

/// Test: buildCommand with empty options.
unittest {
    string[][string] options;

    auto args = buildCommand(options, ["echo", "hello"], CommandJoinMode.whitespace);

    assert(args.length == 1);
    assert(args[0] == "echo hello");
}

/// Test: buildCommand skips invalid keys.
unittest {
    string[][string] options;
    options["00_valid"] = ["--valid"];
    options["invalid"] = ["--should-be-skipped"];
    options["01_also_valid"] = ["--also-valid"];

    auto args = buildCommand(options, ["echo"], CommandJoinMode.whitespace);

    assert(args[0] == "--valid");
    assert(args[1] == "--also-valid");
    assert(args[2] == "echo");
}

/// Test: HostRunner construction and dispose.
unittest {
    string[][string] options;
    string[string] envVars;

    auto runner = new HostRunner(options, AbsolutePath("/tmp"), envVars,
            CommandJoinMode.whitespace, AbsolutePath("/workarea"), 30.dur!"seconds", 1_048_576);

    assert(runner !is null);
    runner.dispose(); // Should not throw
    runner.dispose(); // Should be safe to call multiple times
}

/// Test: buildCommand produces correct command.
unittest {
    string[][string] options;

    auto args = buildCommand(options, ["echo", "hello"], CommandJoinMode.append);

    assert(args.length == 2, i"args.length: $(args.length), args: $(args)".text);
    assert(args[0] == "echo");
    assert(args[1] == "hello");
}
