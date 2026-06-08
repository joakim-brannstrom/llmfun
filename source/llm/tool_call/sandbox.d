module llm.tool_call.sandbox;

import std.file : exists;
import std.format : format;
import std.json : JSONValue;
import std.process : execute;
import std.string : toLower, replace;
import std.conv : to;

import my.path : AbsolutePath, Path;

import llm.tool_call;
import llm.tool_call.utility;

mixin RegisterLlmFunctions!();

interface SandboxContext : Context {
    bool isPathInsideWorkArea(AbsolutePath path);
    AbsolutePath workArea();

    // must have the same syntax as docker so either docker or podman
    string getContainerCmd();
}

@Function("Execute d/python code in sandbox. Returns JSON with exit_code and output")
ExecuteFuncResult executeCode(Context baseCtx, string path, string language) {
    mixin(baseContextToSpecific!SandboxContext);

    auto pathRes = pathToWorkarea(ctx, path, checkExist: true);
    if (!pathRes.valid)
        return ExecuteFuncResult(pathRes.errorMsg, success: false);
    auto path_ = pathRes.path;

    try {
        string cmd;
        const imageName = {
            switch (language.toLower) {
            case "d":
                cmd = "ldmd2 -run /source";
                return "dlang/llmfun:1.0";
            case "python":
                cmd = "python3 /source";
                return "llmfun/python3:1.0";
            default:
                throw new Exception("unsupported language " ~ language);
            }
        }();

        // TODO: check the ulimit config. This is copied from the podman run documentation.
        auto status = execute([
            ctx.getContainerCmd, "run", "--rm", "--cpus", "2", "--ulimit",
            "nofile=1024:1024", "--stop-timeout", "60", "--memory", "1g", "-v",
            path_.toString ~ ":/source:ro", "-v",
            ctx.workArea.toString ~ ":/workarea", imageName, "bash", "-c", cmd
        ]);
        return ExecuteFuncResult(JSONValue([
            "exit_code": JSONValue(status.status),
            "output": JSONValue(status.output)
        ]).toString, success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(format!"error: %s"(e.msg), success: false);
    }
}

@Function("Execute d code with dub in sandbox. Parameter command: build or test. See getThinkingTemplate('dlang_dub') for more information. Returns JSON with exit_code and output")
ExecuteFuncResult executeDCodeWithDub(Context baseCtx, string path, string command) {
    mixin(baseContextToSpecific!SandboxContext);

    auto pathRes = pathToWorkarea(ctx, path, checkExist: true);
    if (!pathRes.valid)
        return ExecuteFuncResult(pathRes.errorMsg, success: false);
    auto path_ = pathRes.path;

    try {
        string cmd;
        switch (command) {
        case "build":
            cmd = "dub build";
            break;
        case "test":
            cmd = "dub test";
            break;
        default:
            return ExecuteFuncResult(
                    "error: supported commands are 'build', 'test'. Unsupported command argument: "
                    ~ command);
        }
        const imageName = "dlang/llmfun:1.0";

        auto status = execute([
            ctx.getContainerCmd, "run", "--rm", "--cpus", "2", "--ulimit",
            "nofile=1024:1024", "--stop-timeout", "60", "--memory", "8g", "-v",
            path_.toString ~ ":/workarea:rw", imageName, "bash", "-c", cmd
        ]);
        return ExecuteFuncResult(JSONValue([
            "exit_code": JSONValue(status.status),
            "output": JSONValue(status.output)
        ]).toString, success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(format!"error: %s"(e.msg), success: false);
    }
}

@Function("Execute a git command in the specified repository directory. Returns JSON with `exit_code` and combined `stdout`/`stderr` in `output`.\n" ~ "command: Git subcommand and arguments (without leading 'git'), e.g. 'status', 'commit -m msg'\n" ~ "repo: Path to the repository directory to run the command in")
ExecuteFuncResult executeGit(Context baseCtx, string repo, string command) {
    mixin(baseContextToSpecific!SandboxContext);

    auto pathRes = pathToWorkarea(ctx, repo, checkExist: true);
    if (!pathRes.valid)
        return ExecuteFuncResult(pathRes.errorMsg.replace("path", "repo"), success: false);
    auto path_ = pathRes.path;

    try {
        const imageName = "llmfun/git:1.0";
        auto status = execute([
            ctx.getContainerCmd, "run", "--rm", "--cpus", "2", "--ulimit",
            "nofile=1024:1024", "--stop-timeout", "60", "--memory", "8g",
            "-v", path_.toString ~ ":/workarea:rw", imageName, "git " ~ command
        ]);
        return ExecuteFuncResult(JSONValue([
            "exit_code": JSONValue(status.status),
            "output": JSONValue(status.output)
        ]).toString, success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(format!"error: %s"(e.msg), success: false);
    }
}
