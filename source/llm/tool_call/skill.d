module llm.tool_call.skill;

import std.array : empty;
import std.conv : text;
import std.file : exists;
import std.format : format;
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
}

@Function("Load a skill by copying it into the sandbox workarea. "
        ~ "Use this when a skill's name or description matches the current task. "
        ~ "Copies the entire skill directory (SKILL.md, references/, scripts/, assets/) "
        ~ "to the destination. Returns the full SKILL.md body. "
        ~ "Fails if the destination already exists and contains a different skill.")
ExecuteFuncResult loadSkill(Context baseCtx, LoadSkillParams params) {
    mixin(baseContextToSpecific!SkillContext);

    auto destDir_ = pathToWorkarea(ctx, params.destDir);
    if (!destDir_.valid) {
        return ExecuteFuncResult(destDir_.errorMsg, success: false);
    }
    if (!ctx.workArea.exists) {
        return ExecuteFuncResult("error: using a skill is blocked. loadSkill is disabled",
                success: false);
    }
    if (params.skillName.strip.empty) {
        return ExecuteFuncResult("error: empty parameter skillName", success: false);
    }

    try {
        auto mgr = ctx.getSkillManager();
        auto body = mgr.loadSkill(params.skillName, destDir_);
        string bodyPreview = body;
        if (body.length > 4096) {
            // TODO: should count grapheme so chinese letters work correctly
            body = body[0 .. 4096] ~ format("\n\n... (truncated, skill is %s bytes total",
                    body.length);
        }
        return ExecuteFuncResult(i"Skill loaded: $(params.skillName)\nCopied to: $(params.destDir)\n\n$(
                bodyPreview)".text, success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(i"error: loading skill '$(params.skillName): $(e.msg)".text,
                success: false);
    }
}
