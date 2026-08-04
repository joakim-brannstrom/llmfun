module llm.tool_call.sandbox;

import logger = std.logger;
import std.algorithm : map, any, canFind, startsWith, endsWith, joiner;
import std.array : appender, join, empty, array;
import std.conv : to, text;
import std.datetime.stopwatch : StopWatch, AutoStart, dur, Duration;
import std.digest : toHexString;
import std.digest.md : md5Of;
import std.exception : collectException;
import std.file : exists;
import std.json : JSONValue, JSONOptions;
import std.range : isInputRange;
import std.string : replace;

import proc;

import my.path : AbsolutePath, Path;

import llm.config : SandboxConfig, ImageCatalogEntry, CatalogState;
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
    /// Standard output from the container
    string stdout;

    /// Standard error from the container. May contain truncation warnings.
    string stderr;

    /// Exit code. Negative values indicate errors (e.g., -1 for timeout).
    int exitCode;
}

/// Shell-quote a string to prevent injection when passed to sh -c.
/// Uses single quotes and escapes existing single quotes.
private string shellQuote(string s) @safe pure nothrow {
    return "'" ~ s.replace("'", "'\"'\"'") ~ "'";
}

/// Build the `docker run` / `podman run` argument array with security flags.
/// Returns the argument array (not including the runtime CLI itself).
/// Precondition: command must not be empty (validated by caller).
string[] buildRunArgs(SandboxConfig config, string image, string[] command) @safe pure {
    // Quote each command element to prevent shell injection.
    // Commands are joined with && for sequential execution under sh -c.
    string joinedCmd = command.map!shellQuote.join(" && ");

    return [
        "run", "--rm", "--user", config.userNs, "--network", "none", "--memory",
        config.memoryLimit, "--cpus", config.cpuLimit, "--read-only", "--tmpfs",
        "/tmp:" ~ config.tmpfsOptions, "--stop-timeout",
        config.timeoutSeconds.to!string, image, "sh", "-c", joinedCmd,
    ];
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
    // because of the logic in the function stdoutApp and stderrApp may contain
    // a couple of bytes more than maxOutputBytes but that is okey because a
    // couple of bytes over the limit is no problem. The limit try to guard
    // against Mbyte of data over the limit. A couple of hundreds of byte is no
    // problem.
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

    // If name contains a path separator, check it directly
    if (canFind(name, dirSeparator)) {
        return exists(name);
    }

    return !whichFromEnv("PATH", name).empty;
}

/// Check if an image name matches an allow-list entry.
/// Supports exact match ("alpine:latest") and prefix match ("python:*").
private bool imageMatchesAllowListEntry(string image, string entry) {
    if (entry.endsWith("*")) {
        // Prefix match: "python:*" matches "python:3.11", "python:latest", etc.
        string prefix = entry[0 .. $ - 1];
        return image.startsWith(prefix);
    }
    // Exact match
    return image == entry;
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
            return ExecuteFuncResult("{\"error\": \"Image catalog failed to load. Using allow-list fallback.\"}",
                    success: true);
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

        // Image validation: catalog check chain
        // loaded → catalog check → allowList → allow all
        if (config.catalogState == CatalogState.loaded) {
            if (config.imageCatalog.empty) {
                logger.warningf("Image rejected: catalog loaded but empty: %s", image);
                return ExecuteFuncResult("error: image catalog is empty. No images are currently available.",
                        success: false);
            }
            if (!imageInCatalog(image, config.imageCatalog)) {
                logger.warningf("Image rejected by catalog: %s", image);
                return ExecuteFuncResult("error: image '" ~ image
                        ~ "' is not in the allowed catalog. Call listImages() to see available options.",
                        success: false);
            }
            logger.tracef("Image allowed by catalog: %s", image);
        } else {
            // Catalog not configured or load failed — fall back to allow-list
            if (config.allowedImages.length > 0) {
                bool allowed = config.allowedImages.any!(a => imageMatchesAllowListEntry(image, a));
                if (!allowed) {
                    logger.warningf("Image rejected by allow-list: %s (allowed: %s)",
                            image, config.allowedImages.joiner(", "));
                    return ExecuteFuncResult("error: image '" ~ image ~ "' not in allowed list",
                            success: false);
                }
                logger.tracef("Image allowed by allow-list: %s", image);
            } else {
                // Both catalog and allow-list empty - allow all
                logger.tracef("Image allowed (no restrictions): %s", image);
            }
        }

        if (params.command.empty) {
            return ExecuteFuncResult("error: command must not be empty", success: false);
        }

        auto runArgs = buildRunArgs(config, image, params.command);

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

/// Check if an image name exists in the catalog using exact match.
private bool imageInCatalog(string imageName, ImageCatalogEntry[] catalog) @safe pure nothrow {
    return catalog.any!(e => e.name == imageName);
}

// Unit tests for buildRunArgs() and collectOutputLimited()
// ─────────────────────────────────────────────────────────────────────────────

/// Test: buildRunArgs with default SandboxConfig includes all security flags.
unittest {
    auto config = SandboxConfig();
    auto args = buildRunArgs(config, "alpine:latest", ["echo", "hello"]);

    assert(args[0] == "run");
    assert(args.canFind("--rm"));
    assert(args.canFind("--user"));
    assert(args.canFind("--network"));
    assert(args.canFind("none"));
    assert(args.canFind("--memory"));
    assert(args.canFind("--cpus"));
    assert(args.canFind("--read-only"));
    assert(args.canFind("--tmpfs"));
    assert(args.canFind("--stop-timeout"));

    // Verify default values are used
    assert(args.canFind("1000:1000")); // userNs
    assert(args.canFind("256m")); // memoryLimit
    // Verify --stop-timeout is followed by "60" (positional check avoids false positives)
    long timeoutIdx = -1;
    foreach (i, arg; args) {
        if (arg == "--stop-timeout") {
            timeoutIdx = i;
            break;
        }
    }
    assert(timeoutIdx >= 0); // --stop-timeout exists
    assert(args[timeoutIdx + 1] == "60"); // timeoutSeconds
    assert(args.canFind("/tmp:rw,noexec,nosuid,size=64m")); // tmpfsOptions

    // Verify image and shell are present
    assert(args.canFind("alpine:latest"));
    assert(args.canFind("sh"));
    assert(args.canFind("-c"));
}

/// Test: buildRunArgs uses custom image when provided.
unittest {
    auto config = SandboxConfig();
    auto args = buildRunArgs(config, "python:3.11", ["python", "--version"]);

    assert(args.canFind("python:3.11"));
    assert(!args.canFind("alpine:latest"));
}

/// Test: buildRunArgs correctly joins and quotes command array elements.
unittest {
    auto config = SandboxConfig();
    auto args = buildRunArgs(config, "alpine:latest", [
        "echo", "hello", "&&", "world"
    ]);

    // Find the command string (last element)
    string cmdStr = args[$ - 1];

    // Each element should be single-quoted
    assert(cmdStr.canFind("'echo'"));
    assert(cmdStr.canFind("'hello'"));
    assert(cmdStr.canFind("'&&'")); // The && in the argument should be quoted, not interpreted
    // Elements are joined with && (the separator between quoted elements)
    assert(cmdStr.startsWith("'echo' && 'hello' && '&&' && 'world'"));
    // Elements should be joined with &&
    assert(cmdStr.canFind("&&"));
}

/// Test: buildRunArgs applies custom config values.
unittest {
    auto config = SandboxConfig();
    config.runtimeCli = "podman";
    config.defaultImage = "ubuntu:22.04";
    config.timeoutSeconds = 120;
    config.memoryLimit = "512m";
    config.cpuLimit = "1.0";
    config.tmpfsOptions = "rw,size=128m";
    config.userNs = "2000:2000";

    auto args = buildRunArgs(config, "ubuntu:22.04", ["bash", "-c", "test"]);

    assert(args.canFind("2000:2000")); // custom userNs
    assert(args.canFind("512m")); // custom memoryLimit
    assert(args.canFind("1.0")); // custom cpuLimit
    assert(args.canFind("120")); // custom timeoutSeconds
    assert(args.canFind("/tmp:rw,size=128m")); // custom tmpfsOptions
    assert(args.canFind("ubuntu:22.04")); // custom image
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

// Unit tests for executeImage() and image allow-list matching

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

/// Test: executeImage returns descriptive error when runtime CLI not found.
unittest {
    auto ctx = new MockSandboxContext();
    ctx.config.runtimeCli = "nonexistent-runtime-cli-12345";

    auto params = ExecuteImageParams();
    params.imageName = "alpine:latest";
    params.command = ["echo", "hello"];

    auto result = executeImage(cast(Context) ctx, params);

    assert(!result.success);
    assert(result.msg.canFind("not found"));
    assert(result.msg.canFind("nonexistent-runtime-cli-12345"));
}

/// Test: executeImage returns error when command is empty.
unittest {
    auto ctx = new MockSandboxContext();
    ctx.config.runtimeCli = "/usr/bin/sh";

    auto params = ExecuteImageParams();
    params.imageName = "alpine:latest";
    params.command = []; // empty command

    auto result = executeImage(cast(Context) ctx, params);

    assert(!result.success);
    assert(result.msg.canFind("command must not be empty"));
}

/// Test: executeImage falls back to defaultImage when imageName is empty.
/// Uses /usr/bin/sh as runtime to pass the existence check; the allow-list
/// rejects the default image so we can verify fallback occurred.
unittest {
    auto ctx = new MockSandboxContext();
    ctx.config.runtimeCli = "/usr/bin/sh";
    ctx.config.defaultImage = "alpine:latest";
    ctx.config.allowedImages = ["ubuntu:*"]; // reject default "alpine:latest"

    auto params = ExecuteImageParams();
    params.imageName = ""; // empty, should fallback to defaultImage
    params.command = ["echo", "hello"];

    auto result = executeImage(cast(Context) ctx, params);

    // Should fail with allow-list error for "alpine:latest" (the default)
    assert(!result.success);
    assert(result.msg.canFind("alpine:latest"));
    assert(result.msg.canFind("not in allowed list"));
}

/// Test: executeImage rejects image not in allow-list.
unittest {
    auto ctx = new MockSandboxContext();
    ctx.config.runtimeCli = "/usr/bin/sh";
    ctx.config.allowedImages = ["ubuntu:*"];

    auto params = ExecuteImageParams();
    params.imageName = "alpine:latest";
    params.command = ["echo", "hello"];

    auto result = executeImage(cast(Context) ctx, params);

    assert(!result.success);
    assert(result.msg.canFind("alpine:latest"));
    assert(result.msg.canFind("not in allowed list"));
}

/// Test: executeImage does NOT reject image that matches allow-list.
/// The container execution will fail (since /usr/bin/sh is not a real runtime),
/// but the error should NOT be an allow-list rejection.
unittest {
    auto ctx = new MockSandboxContext();
    ctx.config.runtimeCli = "/usr/bin/sh";
    ctx.config.allowedImages = ["alpine:*"];

    auto params = ExecuteImageParams();
    params.imageName = "alpine:latest";
    params.command = ["echo", "hello"];

    auto result = executeImage(cast(Context) ctx, params);

    // May fail due to container execution, but NOT due to allow-list
    assert(!result.msg.canFind("not in allowed list"));
}

/// Test: executeImage skips allow-list check when allowedImages is empty.
unittest {
    auto ctx = new MockSandboxContext();
    ctx.config.runtimeCli = "/usr/bin/sh";
    ctx.config.allowedImages = []; // empty = allow all

    auto params = ExecuteImageParams();
    params.imageName = "any-random-image:tag";
    params.command = ["echo", "hello"];

    auto result = executeImage(cast(Context) ctx, params);

    // Should NOT have allow-list error (empty list allows all)
    assert(!result.msg.canFind("not in allowed list"));
}

/// Test: imageMatchesAllowListEntry with exact match.
unittest {
    assert(imageMatchesAllowListEntry("alpine:latest", "alpine:latest"));
    assert(imageMatchesAllowListEntry("python:3.11", "python:3.11"));
    assert(!imageMatchesAllowListEntry("alpine:latest", "alpine:3.18"));
    assert(!imageMatchesAllowListEntry("alpine", "alpine:latest"));
}

/// Test: imageMatchesAllowListEntry with prefix wildcard match.
unittest {
    assert(imageMatchesAllowListEntry("python:3.11", "python:*"));
    assert(imageMatchesAllowListEntry("python:latest", "python:*"));
    assert(imageMatchesAllowListEntry("python:3.11-slim", "python:*"));
    assert(imageMatchesAllowListEntry("ubuntu:22.04", "ubuntu:*"));
    assert(!imageMatchesAllowListEntry("python:3.11", "node:*"));
    assert(!imageMatchesAllowListEntry("my-python:3.11", "python:*"));
}

/// Test: imageInCatalog returns true for exact name match.
unittest {
    ImageCatalogEntry[] catalog = [
        ImageCatalogEntry("alpine:latest", "Minimal Linux", ["linux",
            "minimal"]),
        ImageCatalogEntry("python:3.11-slim", "Python runtime", [
            "python", "dev"
        ]), ImageCatalogEntry("node:20-alpine", "Node.js runtime", ["nodejs"]),
    ];
    assert(imageInCatalog("alpine:latest", catalog));
    assert(imageInCatalog("python:3.11-slim", catalog));
    assert(imageInCatalog("node:20-alpine", catalog));
}

/// Test: imageInCatalog returns false for non-matching name.
unittest {
    ImageCatalogEntry[] catalog = [
        ImageCatalogEntry("alpine:latest", "Minimal Linux", ["linux"]),
    ];
    assert(!imageInCatalog("ubuntu:22.04", catalog));
    assert(!imageInCatalog("alpine:3.18", catalog));
    assert(!imageInCatalog("alpine", catalog));
}

/// Test: imageInCatalog returns false for empty catalog.
unittest {
    ImageCatalogEntry[] catalog;
    assert(!imageInCatalog("alpine:latest", catalog));
    assert(!imageInCatalog("", catalog));
}

// ── Unit tests for executeImage() catalog validation ──────────────────────────

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

/// Test: executeImage with catalog notConfigured falls back to allow-list.
unittest {
    auto ctx = new MockSandboxContext();
    ctx.config.runtimeCli = "/usr/bin/sh";
    ctx.config.catalogState = CatalogState.notConfigured;
    ctx.config.allowedImages = ["ubuntu:*"];

    ExecuteImageParams params;
    params.imageName = "alpine:latest";
    params.command = ["echo", "hello"];

    auto result = executeImage(cast(Context) ctx, params);

    // Should fall back to allow-list and reject
    assert(!result.success);
    assert(result.msg.canFind("not in allowed list"));
}

/// Test: executeImage with catalog loadFailed falls back to allow-list.
unittest {
    auto ctx = new MockSandboxContext();
    ctx.config.runtimeCli = "/usr/bin/sh";
    ctx.config.catalogState = CatalogState.loadFailed;
    ctx.config.allowedImages = ["alpine:*"];

    ExecuteImageParams params;
    params.imageName = "ubuntu:22.04";
    params.command = ["echo", "hello"];

    auto result = executeImage(cast(Context) ctx, params);

    // Should fall back to allow-list and reject
    assert(!result.success);
    assert(result.msg.canFind("not in allowed list"));
}

/// Test: executeImage with both catalog and allow-list empty allows all.
unittest {
    auto ctx = new MockSandboxContext();
    ctx.config.runtimeCli = "/usr/bin/sh";
    ctx.config.catalogState = CatalogState.notConfigured;
    ctx.config.allowedImages = []; // empty = allow all
    ctx.config.imageCatalog = [];

    ExecuteImageParams params;
    params.imageName = "any-random-image:tag";
    params.command = ["echo", "hello"];

    auto result = executeImage(cast(Context) ctx, params);

    // Should NOT have any rejection error
    assert(result.success);
    assert(!result.msg.canFind("not in allowed list"));
    assert(!result.msg.canFind("not in the allowed catalog"));
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

    assert(result.success);
    assert(result.msg.canFind("Image catalog failed to load"));
    assert(result.msg.canFind("allow-list fallback"));
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
