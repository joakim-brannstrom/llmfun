module llm.tool_call.sandbox;

import logger = std.logger;
import std.algorithm : map, any, canFind, startsWith, endsWith, joiner, filter, sort;
import std.array : appender, join, empty, array;
import std.conv : to, text;
import std.datetime.stopwatch : StopWatch, AutoStart, dur, Duration;
import std.digest : toHexString;
import std.digest.md : md5Of;
import std.exception : collectException;
import std.file : exists, thisExePath;
import std.json : JSONValue, JSONOptions;
import std.path : dirName;
import std.range : isInputRange;
import std.string : replace;

import proc;

import my.path : AbsolutePath, Path;

import llm.config : SandboxConfig, ImageCatalogEntry, CatalogState, replaceContainerMagicWords;
import llm.tool_call;
import llm.tool_call.utility;

mixin RegisterLlmFunctions!();

interface SandboxContext : Context {
    bool isPathInsideWorkArea(AbsolutePath path);
    AbsolutePath workArea();

    /// Get the sandbox configuration for container execution
    SandboxConfig getSandboxConfig();
}

/// Structured result from container execution.
/// Negative exit codes (e.g., -1 for timeout) are documented behavior.
struct ContainerResult {
    string stdout;
    string stderr;
    /// Exit code. Negative values indicate errors (e.g., -1 for timeout).
    int exitCode;
}

/// Merge default options with image-specific overrides.
/// Image options completely replace defaults for matching tag keys.
/// Non-overlapping keys from both maps are preserved.
string[][string] mergeOptions(string[][string] defaults, string[][string] overrides) @safe pure nothrow {
    string[][string] result;
    try {
        foreach (key, values; defaults) {
            result[key] = values.dup;
        }
        foreach (key, values; overrides) {
            result[key] = values.dup;
        }
    } catch (Exception e) {
        // fix for ldc-1.40. Remove when min compiler is updated
    }
    return result;
}

/// Test: mergeOptions with empty defaults and empty overrides returns empty.
unittest {
    string[][string] defaults;
    string[][string] overrides;
    auto result = mergeOptions(defaults, overrides);
    assert(result.length == 0);
}

/// Test: mergeOptions with defaults only returns copy of defaults.
unittest {
    string[][string] defaults;
    defaults["security"] = ["--read-only"];
    defaults["network"] = ["--network", "none"];
    string[][string] overrides;
    auto result = mergeOptions(defaults, overrides);
    assert(result.length == 2);
    assert(result["security"] == ["--read-only"]);
    assert(result["network"] == ["--network", "none"]);
}

/// Test: mergeOptions with overrides only returns copy of overrides.
unittest {
    string[][string] defaults;
    string[][string] overrides;
    overrides["mounts"] = ["-v", "/host:/container"];
    auto result = mergeOptions(defaults, overrides);
    assert(result.length == 1);
    assert(result["mounts"] == ["-v", "/host:/container"]);
}

/// Test: mergeOptions with overlapping keys — override completely replaces defaults.
unittest {
    string[][string] defaults;
    defaults["security"] = ["--read-only"];
    defaults["network"] = ["--network", "none"];
    string[][string] overrides;
    overrides["security"] = ["--privileged"];
    auto result = mergeOptions(defaults, overrides);
    assert(result.length == 2);
    assert(result["security"] == ["--privileged"]);
    assert(result["network"] == ["--network", "none"]);
}

/// Test: mergeOptions with non-overlapping keys — both preserved.
unittest {
    string[][string] defaults;
    defaults["security"] = ["--read-only"];
    string[][string] overrides;
    overrides["mounts"] = ["-v", "/host:/container"];
    auto result = mergeOptions(defaults, overrides);
    assert(result.length == 2);
    assert(result["security"] == ["--read-only"]);
    assert(result["mounts"] == ["-v", "/host:/container"]);
}

/// Test: mergeOptions produces independent copies (no aliasing).
unittest {
    string[][string] defaults;
    defaults["security"] = ["--read-only"];
    string[][string] overrides;
    auto result = mergeOptions(defaults, overrides);
    // Modify original and verify result is unchanged
    defaults["security"] = ["--privileged"];
    assert(result["security"] == ["--read-only"]);
}

/// Test: mergeOptions produces independent copies of inner arrays.
unittest {
    string[][string] defaults;
    defaults["security"] = ["--read-only"];
    string[][string] overrides;
    auto result = mergeOptions(defaults, overrides);
    // Modify the inner array of the original defaults and verify result is unchanged
    defaults["security"][0] = "--privileged";
    assert(result["security"][0] == "--read-only");
}

/// Build the `docker run` / `podman run` argument array from options maps.
/// Pipeline: merge defaults with image options -> substitute magic words -> flatten to CLI args.
/// Returns the argument array (not including the runtime CLI itself).
/// Params:
///     config = Sandbox configuration with defaultOptions
///     workArea = Workarea path for @{llmfun_workarea} substitution
///     entry = Image catalog entry with per-image options
///     command = Command to execute inside container
/// Precondition: command must not be empty (validated by caller).
string[] buildRunArgs(SandboxConfig config, AbsolutePath workArea,
        ImageCatalogEntry entry, string[] command) @safe nothrow {
    auto merged = mergeOptions(config.defaultOptions, entry.options);
    auto resolved = replaceContainerMagicWords(merged, workArea);
    string[] args;
    auto sortedKeys = resolved.keys.array.sort!((a, b) => a < b);
    foreach (key; sortedKeys) {
        if (key.length < 3 || key[2] != '_') {
            try {
                logger.warningf("Skipping option key '%s': missing numeric prefix", key);
            } catch (Exception) {
            }
            continue;
        }
        try {
            int prefix = key[0 .. 2].to!int;
        } catch (Exception e) {
            try {
                logger.warningf("Skipping option key '%s': invalid numeric prefix", key);
            } catch (Exception) {
            }
            continue;
        }
        auto values = resolved[key];
        if (!values.empty) {
            args ~= values;
        }
    }

    args ~= [entry.name, "sh", "-c", command.join(" ")];

    return args;
}

/// Result from collecting limited output from a process.
private struct CollectedOutput {
    string stdout;
    string stderr;
}

/// Collect output from a proc drain range with per-stream byte limits.
/// Appends truncation warnings to stderr when limits are exceeded.
/// Continues draining after truncation to prevent pipe blocking.
/// Note: Assumes output is text (UTF-8). Binary output will produce garbage.
/// Template accepts any range of DrainElement (streaming range or materialized array).
CollectedOutput collectOutputLimited(R)(R elems, long maxOutputBytes) @safe
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

    // Capture truncation state before appending warnings to avoid
    // stdout warning bytes counting toward stderr limit.
    bool stdoutTruncated = stdoutApp.length >= maxOutputBytes;
    bool stderrTruncated = stderrApp.length >= maxOutputBytes;

    if (stdoutTruncated) {
        stderrApp.put(cast(const(ubyte)[])("\n[stdout truncated: " ~ stdoutApp.length.to!string
                ~ " bytes collected, limit: " ~ maxOutputBytes.to!string ~ " bytes]"));
    }
    if (stderrTruncated) {
        stderrApp.put(cast(const(ubyte)[])("\n[stderr truncated: " ~ stderrApp.length.to!string
                ~ " bytes collected, limit: " ~ maxOutputBytes.to!string ~ " bytes]"));
    }

    return CollectedOutput(stdoutApp.data[].idup, stderrApp.data[].idup);
}

/// Check if an executable exists in PATH.
/// Returns true if the executable can be found, false otherwise.
private bool canFindExecutable(string name) {
    import std.path : dirSeparator;
    import my.file : whichFromEnv;

    if (canFind(name, dirSeparator)) {
        return exists(name);
    }

    return !whichFromEnv("PATH", name).empty;
}

/// Core container execution function using the proc library.
/// Spawns the container runtime with the given arguments, enforces timeout,
/// collects output with per-stream byte limits, and returns structured results.
///
/// Params:
///    runtimeCli = Container runtime CLI (e.g., "docker" or "podman")
///    args = Arguments to pass to the runtime (e.g., ["run", "--rm", ...])
///    timeoutSec = Execution timeout in seconds
///    maxOutputBytes = Maximum bytes per output stream
///    image = Container image name (for logging)
/// Returns: ContainerResult with stdout, stderr, and exitCode
ContainerResult runContainerCommand(string runtimeCli, string[] args,
        Duration timeout, long maxOutputBytes, string image) nothrow {
    int exitCode = -1;
    CollectedOutput output;
    auto sw = StopWatch(AutoStart.yes);
    scope (exit)
        sw.stop();
    try {
        auto fullCmd = [runtimeCli] ~ args;

        auto p = proc.pipeProcess(fullCmd).sandbox.timeout(timeout);
        scope (exit)
            p.dispose;

        output = collectOutputLimited(proc.drain(p), maxOutputBytes);

        try {
            exitCode = p.wait;
        } catch (Exception e) {
            return ContainerResult(output.stdout, output.stderr ~ "\nerror: " ~ e.msg, -1);
        }
    } catch (Exception e) {
        try {
            return ContainerResult(null, i"error: running container with arguments $(args): $(e.msg)".text,
                    -1);
        } catch (Exception e) {
        }
    }
    // Log timeout kill if detected (negative exit code indicates signal termination)
    if (exitCode < 0 && exitCode != -1) {
        logger.tracef("Container killed: image=%s, exitCode=%d, elapsed=%s",
                image, exitCode, sw.peek).collectException;
    }

    return ContainerResult(output.stdout, output.stderr, exitCode);
}

/// Check if runtime CLI exists and return error message if not found.
private string checkRuntimeCli(string runtimeCli) {
    if (!canFindExecutable(runtimeCli)) {
        return "error: container runtime '" ~ runtimeCli ~ "' not found in PATH";
    }
    return "";
}

struct ListImagesParams {
}

@Function("List available container images from the curated catalog. Returns a JSON array of image entries with name, description, and tags.")
ExecuteFuncResult listImages(Context baseCtx, ListImagesParams params) nothrow {
    mixin(baseContextToSpecific!SandboxContext);

    try {
        auto config = ctx.getSandboxConfig();

        if (config.catalogState == CatalogState.loadFailed) {
            return ExecuteFuncResult("error: no image catalog loaded. executeImage is unavailable",
                    success: false);
        }
        if (config.catalogState != CatalogState.loaded) {
            return ExecuteFuncResult("[]", success: true);
        }

        JSONValue[] entries;
        foreach (entry; config.imageCatalog) {
            JSONValue obj;
            obj["name"] = entry.name;
            obj["description"] = entry.description;
            obj["tags"] = JSONValue(entry.tags);
            entries ~= obj;
        }

        return ExecuteFuncResult(JSONValue(entries)
                .toString(JSONOptions.doNotEscapeSlashes), success: true);
    } catch (Exception e) {
        return ExecuteFuncResult("error: " ~ e.msg, success: false);
    }
}

struct ExecuteImageParams {
    @ParamDescription("Container image to run (e.g., 'alpine:latest', 'python:3.11')")
    string imageName;

    @ParamDescription("Command elements to execute inside the container")
    string[] command;
}

@Function("Execute a command in a container image. Returns JSON with exit_code, stdout, and stderr")
ExecuteFuncResult executeImage(Context baseCtx, ExecuteImageParams params) nothrow {
    mixin(baseContextToSpecific!SandboxContext);

    try {
        auto config = ctx.getSandboxConfig();

        string e = checkRuntimeCli(config.runtimeCli);
        if (!e.empty) {
            logger.warningf("Runtime CLI not found: %s: %s", config.runtimeCli, e);
            return ExecuteFuncResult(e, success: false);
        }

        string image = params.imageName.empty ? config.defaultImage : params.imageName;
        ImageCatalogEntry entry;
        bool foundImage;
        foreach (a; config.imageCatalog.filter!(a => a.name == image)) {
            entry = a;
            foundImage = true;
        }

        // Image validation: catalog check chain
        // loaded → catalog check → allowList → allow all
        if (config.catalogState == CatalogState.loaded) {
            if (config.imageCatalog.empty) {
                logger.warningf("Image rejected: catalog loaded but empty: %s", image);
                return ExecuteFuncResult("error: image catalog is empty. No images are currently available.",
                        success: false);
            }
            if (!foundImage) {
                logger.warningf("Image rejected by catalog: %s", image);
                return ExecuteFuncResult("error: image '" ~ image
                        ~ "' is not in the allowed catalog. Call listImages() to see available options.",
                        success: false);
            }
            logger.tracef("Image allowed by catalog: %s", image);
        } else {
            logger.trace("Image rejected, no catalog loaded: %s", image);
            return ExecuteFuncResult("error: no catalog loaded. No images allowed to execute",
                    success: false);
        }

        if (params.command.empty) {
            return ExecuteFuncResult("error: command must not be empty", success: false);
        }

        auto runArgs = buildRunArgs(config, ctx.workArea, entry, params.command);

        string cmdHashShort = toHexString(md5Of(params.command.join(" "))).idup[0 .. 8];

        auto sw = StopWatch(AutoStart.yes);
        logger.tracef("Container start: image=%s, cmdHash=%s, runtime=%s",
                image, cmdHashShort, config.runtimeCli);
        auto result = runContainerCommand(config.runtimeCli, runArgs,
                config.timeoutSeconds.dur!"seconds", config.maxOutputBytes, image);
        sw.stop();
        logger.tracef("Container end: image=%s, exitCode=%s, duration=%s, stdout=%s bytes, stderr=%s bytes", image,
                result.exitCode, sw.peek, result.stdout.length, result.stderr.length);

        if (result.stderr.canFind("truncated")) {
            logger.tracef("Output truncated: image=%s, stdout=%d bytes, stderr=%d bytes, limit=%d bytes", image,
                    result.stdout.length, result.stderr.length, config.maxOutputBytes);
        }

        return ExecuteFuncResult(JSONValue([
            "exit_code": JSONValue(result.exitCode),
            "stdout": JSONValue(result.stdout),
            "stderr": JSONValue(result.stderr)
        ]).toString(JSONOptions.doNotEscapeSlashes), success: true);
    } catch (Exception e) {
        return ExecuteFuncResult("error: image '" ~ params.imageName ~ "' failed to execute: " ~ e.msg,
                success: false);
    }
}

// Unit tests for buildRunArgs() and collectOutputLimited()
// ─────────────────────────────────────────────────────────────────────────────

/// Test: empty options produces [image, "sh", "-c", cmd].
unittest {
    auto config = SandboxConfig();
    config.defaultOptions["00_subcommand"] = ["run"];
    ImageCatalogEntry entry;
    entry.name = "alpine:latest";
    auto args = buildRunArgs(config, AbsolutePath("/workarea"), entry, [
        "echo", "hello"
    ]);

    assert(args == ["run", "alpine:latest", "sh", "-c", "echo hello"]);
}

/// Test: tag with empty value array produces no output.
unittest {
    auto config = SandboxConfig();
    config.defaultOptions["00_subcommand"] = ["run"];
    config.defaultOptions["99_empty"] = [];
    ImageCatalogEntry entry;
    entry.name = "alpine:latest";
    auto args = buildRunArgs(config, AbsolutePath("/workarea"), entry, [
        "echo", "hi"
    ]);

    assert(args == ["run", "alpine:latest", "sh", "-c", "echo hi"]);
}

/// Test: tag with CLI args produces flattened output.
unittest {
    auto config = SandboxConfig();
    config.defaultOptions["00_subcommand"] = ["run"];
    config.defaultOptions["01_cleanup"] = ["--rm"];
    config.defaultOptions["02_security"] = ["--read-only"];
    config.defaultOptions["03_network"] = ["--network", "none"];
    ImageCatalogEntry entry;
    entry.name = "alpine:latest";
    auto args = buildRunArgs(config, AbsolutePath("/workarea"), entry, [
        "echo", "hello"
    ]);

    assert(args[0] == "run");
    assert(args.canFind("--rm"));
    assert(args.canFind("--read-only"));
    assert(args.canFind("--network"));
    assert(args.canFind("none"));
    assert(args.canFind("alpine:latest"));
    assert(args.canFind("sh"));
    assert(args.canFind("-c"));
}

/// Test: magic words are substituted in output.
unittest {
    auto config = SandboxConfig();
    config.defaultOptions["00_subcommand"] = ["run"];
    config.defaultOptions["05_mounts"] = [
        "-v", "@{llmfun_workarea}:/workarea", "-v", "@{llmfun}/data:/data"
    ];
    ImageCatalogEntry entry;
    entry.name = "alpine:latest";
    auto args = buildRunArgs(config, AbsolutePath("/my/work"), entry, [
        "echo", "hello"
    ]);

    assert(args.canFind("/my/work:/workarea"));
    assert(args.canFind(i"$(thisExePath.dirName)/data:/data".text));
}

/// Test: image options override default options for same tag key.
unittest {
    auto config = SandboxConfig();
    config.defaultOptions["00_subcommand"] = ["run"];
    config.defaultOptions["03_network"] = ["--network", "none"];
    config.defaultOptions["02_security"] = ["--read-only"];
    ImageCatalogEntry entry;
    entry.name = "alpine:latest";
    entry.options["03_network"] = ["--network", "host"];
    auto args = buildRunArgs(config, AbsolutePath("/workarea"), entry, [
        "echo", "hello"
    ]);

    // Image override: network should be "host", not "none"
    assert(args.canFind("host"));
    assert(!args.canFind("none"));
    // Default preserved: security should still be present
    assert(args.canFind("--read-only"));
}

/// Test: image name and command appended correctly.
unittest {
    auto config = SandboxConfig();
    config.defaultOptions["00_subcommand"] = ["run"];
    ImageCatalogEntry entry;
    entry.name = "python:3.11";
    auto args = buildRunArgs(config, AbsolutePath("/workarea"), entry, [
        "python", "--version"
    ]);

    assert(args[$ - 4] == "python:3.11");
    assert(args[$ - 3] == "sh");
    assert(args[$ - 2] == "-c");
    assert(args[$ - 1] == "python --version");
}

/// Test: command array elements joined with spaces.
unittest {
    auto config = SandboxConfig();
    config.defaultOptions["00_subcommand"] = ["run"];
    ImageCatalogEntry entry;
    entry.name = "alpine:latest";
    auto args = buildRunArgs(config, AbsolutePath("/workarea"), entry, [
        "echo", "hello", "&&", "world"
    ]);

    string cmdStr = args[$ - 1];
    assert(cmdStr == "echo hello && world");
}

// ── Numeric prefix ordering tests ──────────────────────────────────────────

/// Test: numeric-prefixed keys sort correctly (00 < 01 < 02 < 03).
unittest {
    auto config = SandboxConfig();
    config.defaultOptions["03_network"] = ["--network", "none"];
    config.defaultOptions["00_subcommand"] = ["run"];
    config.defaultOptions["01_cleanup"] = ["--rm"];
    config.defaultOptions["02_security"] = ["--read-only"];
    ImageCatalogEntry entry;
    entry.name = "alpine:latest";
    auto args = buildRunArgs(config, AbsolutePath("/workarea"), entry, [
        "echo", "hello"
    ]);

    // Verify exact order: 00, 01, 02, 03, image, sh, -c, cmd
    assert(args[0] == "run");
    assert(args[1] == "--rm");
    assert(args[2] == "--read-only");
    assert(args[3] == "--network");
    assert(args[4] == "none");
    assert(args[5] == "alpine:latest");
}

/// Test: numeric-prefixed keys are processed correctly.
unittest {
    auto config = SandboxConfig();
    config.defaultOptions["00_subcommand"] = ["run"];
    config.defaultOptions["01_cleanup"] = ["--rm"];
    // This test verifies that numeric-prefixed keys are processed correctly.
    // Non-numeric keys would be skipped by the validation in buildRunArgs.
    ImageCatalogEntry entry;
    entry.name = "alpine:latest";
    auto args = buildRunArgs(config, AbsolutePath("/workarea"), entry, [
        "echo", "hello"
    ]);

    // Only numeric-prefixed keys should appear
    assert(args[0] == "run");
    assert(args[1] == "--rm");
    assert(args[2] == "alpine:latest");
}

/// Test: mixed prefix ordering with many keys.
unittest {
    auto config = SandboxConfig();
    config.defaultOptions["05_mounts"] = ["-v", "/host:/container"];
    config.defaultOptions["00_subcommand"] = ["run"];
    config.defaultOptions["03_network"] = ["--network", "none"];
    config.defaultOptions["01_cleanup"] = ["--rm"];
    config.defaultOptions["04_tmpfs"] = ["--tmpfs", "/tmp"];
    config.defaultOptions["02_security"] = ["--read-only"];
    ImageCatalogEntry entry;
    entry.name = "alpine:latest";
    auto args = buildRunArgs(config, AbsolutePath("/workarea"), entry, [
        "echo", "hello"
    ]);

    // Verify order: 00, 01, 02, 03, 04, 05, image, sh, -c, cmd
    assert(args[0] == "run");
    assert(args[1] == "--rm");
    assert(args[2] == "--read-only");
    assert(args[3] == "--network");
    assert(args[4] == "none");
    assert(args[5] == "--tmpfs");
    assert(args[6] == "/tmp");
    assert(args[7] == "-v");
    assert(args[8] == "/host:/container");
    assert(args[9] == "alpine:latest");
}

/// Test: two-digit numeric prefixes sort correctly (09 < 10 < 99).
unittest {
    auto config = SandboxConfig();
    config.defaultOptions["10_late"] = ["--late"];
    config.defaultOptions["00_subcommand"] = ["run"];
    config.defaultOptions["99_final"] = ["--final"];
    config.defaultOptions["09_early"] = ["--early"];
    ImageCatalogEntry entry;
    entry.name = "alpine:latest";
    auto args = buildRunArgs(config, AbsolutePath("/workarea"), entry, [
        "echo", "hello"
    ]);

    // Verify order: 00, 09, 10, 99, image, sh, -c, cmd
    assert(args[0] == "run");
    assert(args[1] == "--early");
    assert(args[2] == "--late");
    assert(args[3] == "--final");
    assert(args[4] == "alpine:latest");
}

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
    // Create multiple elements that together exceed the small limit
    // Each element is 50 bytes, limit is 100 bytes
    ubyte[50] chunk;
    chunk[] = cast(ubyte) 'a';

    DrainElement[] elems = [
        DrainElement(DrainElement.Type.stdout, cast(const(ubyte)[]) chunk[]),
        DrainElement(DrainElement.Type.stdout, cast(const(ubyte)[]) chunk[]),
        DrainElement(DrainElement.Type.stdout, cast(const(ubyte)[]) chunk[]),
        DrainElement(DrainElement.Type.stdout, cast(const(ubyte)[]) chunk[]), // 200 total, should stop at ~100
    ];

    auto result = collectOutputLimited(elems, 100);

    // Output should be truncated near the limit (allow small overflow per design)
    assert(result.stdout.length >= 100); // At least close to the limit
    assert(result.stdout.length <= 150); // Well under total input size (150 bytes)

    // Stderr should contain truncation warning
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

// Unit tests for executeImage()

version (unittest) {
    private class MockSandboxContext : SandboxContext {
        SandboxConfig config;

        bool isPathInsideWorkArea(AbsolutePath path) {
            return true;
        }

        AbsolutePath workArea() {
            return AbsolutePath("/tmp/test-workarea");
        }

        SandboxConfig getSandboxConfig() {
            return config;
        }
    }
}

// ── Unit tests for executeImage() catalog validation ──────────────────────────

/// Test: executeImage falls back to defaultImage cataloge image when imageName is empty.
/// Uses /usr/bin/sh as runtime to pass the existence check
unittest {
    auto ctx = new MockSandboxContext();
    ctx.config.runtimeCli = "/usr/bin/sh";
    ctx.config.defaultImage = "alpine:latest";
    ctx.config.catalogState = CatalogState.loaded;
    ctx.config.imageCatalog = [
        ImageCatalogEntry("alpine:latest", "Minimal Linux", ["linux"]),
        ImageCatalogEntry("python:3.11-slim", "Python runtime", ["python"]),
    ];

    ExecuteImageParams params;
    params.command = ["echo", "hello"];

    auto result = executeImage(cast(Context) ctx, params);

    // Should pass for "alpine:latest" (the default)
    assert(result.success);
    assert(result.msg.canFind("/usr/bin/sh"));
}

/// Test: executeImage with catalog loaded allows image in catalog.
unittest {
    auto ctx = new MockSandboxContext();
    ctx.config.runtimeCli = "/usr/bin/sh";
    ctx.config.catalogState = CatalogState.loaded;
    ctx.config.imageCatalog = [
        ImageCatalogEntry("alpine:latest", "Minimal Linux", ["linux"]),
        ImageCatalogEntry("python:3.11-slim", "Python runtime", ["python"]),
    ];

    ExecuteImageParams params;
    params.imageName = "alpine:latest";
    params.command = ["echo", "hello"];

    auto result = executeImage(cast(Context) ctx, params);

    // Should NOT have catalog rejection error
    assert(result.success);
    assert(!result.msg.canFind("not in the allowed catalog"));
}

/// Test: executeImage with catalog loaded rejects image not in catalog.
unittest {
    auto ctx = new MockSandboxContext();
    ctx.config.runtimeCli = "/usr/bin/sh";
    ctx.config.catalogState = CatalogState.loaded;
    ctx.config.imageCatalog = [
        ImageCatalogEntry("alpine:latest", "Minimal Linux", ["linux"]),
    ];

    ExecuteImageParams params;
    params.imageName = "ubuntu:22.04";
    params.command = ["echo", "hello"];

    auto result = executeImage(cast(Context) ctx, params);

    assert(!result.success);
    assert(result.msg.canFind("ubuntu:22.04"));
    assert(result.msg.canFind("not in the allowed catalog"));
    assert(result.msg.canFind("listImages()"));
}

/// Test: executeImage with empty loaded catalog denies all images.
unittest {
    auto ctx = new MockSandboxContext();
    ctx.config.runtimeCli = "/usr/bin/sh";
    ctx.config.catalogState = CatalogState.loaded;
    ctx.config.imageCatalog = []; // empty catalog

    ExecuteImageParams params;
    params.imageName = "alpine:latest";
    params.command = ["echo", "hello"];

    auto result = executeImage(cast(Context) ctx, params);

    assert(!result.success);
    assert(result.msg.canFind("image catalog is empty"));
}

/// Test: executeImage with catalog notConfigured
unittest {
    auto ctx = new MockSandboxContext();
    ctx.config.runtimeCli = "/usr/bin/sh";
    ctx.config.catalogState = CatalogState.notConfigured;

    ExecuteImageParams params;
    params.imageName = "alpine:latest";
    params.command = ["echo", "hello"];

    auto result = executeImage(cast(Context) ctx, params);

    // No catalog and use of executeImage is blocked
    assert(!result.success);
    assert(result.msg.canFind("no catalog loaded"));
}

/// Test: executeImage with catalog loadFailed
unittest {
    auto ctx = new MockSandboxContext();
    ctx.config.runtimeCli = "/usr/bin/sh";
    ctx.config.catalogState = CatalogState.loadFailed;

    ExecuteImageParams params;
    params.imageName = "ubuntu:22.04";
    params.command = ["echo", "hello"];

    auto result = executeImage(cast(Context) ctx, params);

    // No valid catalog and use of executeImage is blocked
    assert(!result.success);
    assert(result.msg.canFind("no catalog loaded"));
}

// ── Unit tests for listImages() ───────────────────────────────────────────────

/// Test: listImages returns JSON array when catalog is loaded.
unittest {
    auto ctx = new MockSandboxContext();
    ctx.config.catalogState = CatalogState.loaded;
    ctx.config.imageCatalog = [
        ImageCatalogEntry("alpine:latest", "Minimal Linux", ["linux"]),
        ImageCatalogEntry("python:3.11-slim", "Python runtime", ["python"]),
    ];

    auto result = listImages(cast(Context) ctx, ListImagesParams());

    assert(result.success);
    assert(result.msg.canFind("alpine:latest"));
    assert(result.msg.canFind("python:3.11-slim"));
}

/// Test: listImages returns empty array when catalog is not configured.
unittest {
    auto ctx = new MockSandboxContext();
    ctx.config.catalogState = CatalogState.notConfigured;

    auto result = listImages(cast(Context) ctx, ListImagesParams());

    assert(result.success);
    assert(result.msg == "[]");
}

/// Test: listImages returns error message when catalog failed to load.
unittest {
    auto ctx = new MockSandboxContext();
    ctx.config.catalogState = CatalogState.loadFailed;

    auto result = listImages(cast(Context) ctx, ListImagesParams());

    assert(!result.success);
    assert(result.msg.canFind("no image catalog loaded"));
}

/// Test: listImages returns empty array when catalog is loaded but empty.
unittest {
    auto ctx = new MockSandboxContext();
    ctx.config.catalogState = CatalogState.loaded;
    ctx.config.imageCatalog = [];

    auto result = listImages(cast(Context) ctx, ListImagesParams());

    assert(result.success);
    assert(result.msg == "[]");
}
