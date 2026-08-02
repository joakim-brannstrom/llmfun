module llm.tool_call.sandbox;

import std.conv : to, text;
import std.file : exists;
import std.json : JSONValue, JSONOptions;
import std.process : execute;
import std.string : toLower, replace;

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

struct ExecuteCodeParams {
    @ParamDescription("Path to the source file to execute")
    string path;

    @ParamDescription("Programming language: 'd' or 'python'")
    string language;
}

@Function("Execute code in sandbox. Returns JSON with exit_code and output")
ExecuteFuncResult executeCode(Context baseCtx, ExecuteCodeParams params) {
    mixin(baseContextToSpecific!SandboxContext);

    auto pathRes = pathToWorkarea(ctx, params.path, checkExist: true);
    if (!pathRes.valid)
        return ExecuteFuncResult(pathRes.errorMsg, success: false);
    auto path_ = pathRes.path;

    try {
        string cmd;
        const imageName = {
            switch (params.language.toLower) {
            case "d":
                cmd = "ldmd2 -run /source";
                return "dlang/llmfun:1.0";
            case "python":
                cmd = "python3 /source";
                return "llmfun/python3:1.0";
            default:
                throw new Exception(i"unsupported language $(params.language)".text);
            }
        }();

        // TODO: check the ulimit config. This is copied from the podman run documentation.
        auto status = execute([
            ctx.getContainerCmd, "run", "--rm", "--cpus", "2", "--ulimit",
            "nofile=1024:1024", "--stop-timeout", "60", "--memory", "1g",
            "-v", i"$(path_.toString):/source:ro".text, "-v",
            i"$(ctx.workArea.toString):/workarea".text, imageName, "bash", "-c",
            cmd
        ]);
        return ExecuteFuncResult(JSONValue([
            "exit_code": JSONValue(status.status),
            "output": JSONValue(status.output)
        ]).toString(JSONOptions.doNotEscapeSlashes), success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(i"error: $(e.msg)".text, success: false);
    }
}

struct ExecuteDCodeWithDubParams {
    @ParamDescription("Path to the D project directory containing dub.sdl or dub.json")
    string path;

    @ParamDescription("Command to execute: 'build' or 'test'")
    string command;
}

@Function("Execute d code with dub in sandbox. Always load the skill 'dlang' before use. Returns JSON with exit_code and output")
ExecuteFuncResult executeDCodeWithDub(Context baseCtx, ExecuteDCodeWithDubParams params) {
    mixin(baseContextToSpecific!SandboxContext);

    auto pathRes = pathToWorkarea(ctx, params.path, checkExist: true);
    if (!pathRes.valid)
        return ExecuteFuncResult(pathRes.errorMsg, success: false);
    auto path_ = pathRes.path;

    try {
        string cmd;
        switch (params.command) {
        case "build":
            cmd = "dub build";
            break;
        case "test":
            cmd = "dub test";
            break;
        default:
            return ExecuteFuncResult(i"error: supported commands are 'build', 'test'. Unsupported command argument: $(
                    params.command)".text, success: false);
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
        ]).toString(JSONOptions.doNotEscapeSlashes), success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(i"error: $(e.msg)".text, success: false);
    }
}

struct ExecuteGitParams {
    @ParamDescription("Path to the repository directory to run the command in")
    string repo;

    @ParamDescription(
            "Git subcommand and arguments (without leading 'git'), e.g. 'status', 'commit -m msg'")
    string command;
}

@Function(
        "Execute a git command. Returns JSON with `exit_code` and combined `stdout`/`stderr` in `output`")
ExecuteFuncResult executeGit(Context baseCtx, ExecuteGitParams params) {
    mixin(baseContextToSpecific!SandboxContext);

    auto pathRes = pathToWorkarea(ctx, params.repo, checkExist: true);
    if (!pathRes.valid)
        return ExecuteFuncResult(pathRes.errorMsg.replace("path", "repo"), success: false);
    auto path_ = pathRes.path;

    try {
        const imageName = "llmfun/git:1.0";
        auto status = execute([
            ctx.getContainerCmd, "run", "--rm", "--cpus", "2", "--ulimit",
            "nofile=1024:1024", "--stop-timeout", "60", "--memory", "8g",
            "-v", i"$(path_.toString):/workarea:rw".text, imageName,
            i"git $(params.command)".text
        ]);
        return ExecuteFuncResult(JSONValue([
            "exit_code": JSONValue(status.status),
            "output": JSONValue(status.output)
        ]).toString(JSONOptions.doNotEscapeSlashes), success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(i"error: $(e.msg)".text, success: false);
    }
}
