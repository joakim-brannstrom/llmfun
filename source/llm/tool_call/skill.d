module llm.tool_call.skill;

import logger = std.logger;
import std.array : empty;
import std.conv : text;
import std.file : exists;
import std.string : strip;

import my.path : AbsolutePath;

import llm.skill;
import llm.tool_call.utility;
import llm.tool_call;

mixin RegisterLlmFunctions!();

interface SkillContext : Context {
    SkillManager getSkillManager();
    AbsolutePath workArea();
    bool isPathInsideWorkArea(AbsolutePath path);
}

struct LoadSkillParams {
    @ParamDescription("The name of the skill to load")
    string skillName;

    @ParamDescription(
            "Destination directory path inside the sandbox where the skill will be copied")
    string destDir;

    @ParamOptional @ParamDescription(
            "Force overwrite existing skill regardless of version (default: false)")
    bool overwrite;
}

@Function("Load a skill by copying it into the workarea. "
        ~ "Use this when a skill's name or description matches the current task. "
        ~ "Copies the entire skill directory (SKILL.md, references/, scripts/, assets/) "
        ~ "to the destination. Returns the full SKILL.md body.")
ExecuteFuncResult loadSkill(Context baseCtx, LoadSkillParams params) {
    mixin(baseContextToSpecific!SkillContext);

    auto fallbackLoad() {
        try {
            // fallback to only loading the body but not the full skill
            auto mgr = ctx.getSkillManager();
            auto body = mgr.loadSkillBody(params.skillName);
            return ExecuteFuncResult(i"Skill loaded: $(params.skillName)\nFailed to copy skill to: $(
                    params.destDir)\n\n$(body)".text, success: true);
        } catch (Exception e) {
            return ExecuteFuncResult(i"error: loading skill '$(params.skillName): $(e.msg)".text,
                    success: false);
        }
    };

    if (params.skillName.strip.empty) {
        return ExecuteFuncResult("error: empty parameter skillName", success: false);
    }

    if (!ctx.workArea.exists) {
        return fallbackLoad();
    }

    auto destDir_ = pathToWorkarea(ctx, params.destDir);
    if (!destDir_.valid) {
        return ExecuteFuncResult(destDir_.errorMsg, success: false);
    }

    try {
        // try to load the full skill
        auto mgr = ctx.getSkillManager();
        auto body = mgr.loadSkill(params.skillName, destDir_, params.overwrite);
        if (body.length > 4096) {
            // TODO: should count grapheme so chinese letters work correctly
            body = i"$(body[0 .. 4096])\n\n... (truncated, skill is $(body.length) bytes total)"
                .text;
        }
        return ExecuteFuncResult(i"Skill loaded: $(params.skillName)\nCopied to: $(params.destDir)\n\n$(
                body)".text, success: true);
    } catch (Exception e) {
        logger.trace(e.msg);
    }

    return fallbackLoad();
}
