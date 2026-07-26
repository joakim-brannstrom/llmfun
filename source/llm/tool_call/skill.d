module llm.tool_call.skill;

import std.format : format;
import std.array : empty;
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

@Function("Load a skill by copying it into the sandbox workarea. "
        ~ "Takes the skill name and a destination directory path inside the sandbox. "
        ~ "Copies the entire skill directory (SKILL.md, references/, scripts/, assets/) "
        ~ "to the destination. Returns the full SKILL.md body. "
        ~ "Fails if the destination already exists and contains a different skill. "
        ~ "Use this when a skill's name or description matches the current task.")
ExecuteFuncResult loadSkill(Context baseCtx, string skillName, string destDir) {
    mixin(baseContextToSpecific!SkillContext);

    auto destDir_ = pathToWorkarea(ctx, destDir);
    if (!destDir_.valid) {
        return ExecuteFuncResult(destDir_.errorMsg, success: false);
    }
    if (!ctx.workArea.exists) {
        return ExecuteFuncResult("error: using a skill is blocked. loadSkill is disabled",
                success: false);
    }
    if (skillName.strip.empty) {
        return ExecuteFuncResult("error: empty parameter skillName", success: false);
    }

    try {
        auto mgr = ctx.getSkillManager();
        auto body = mgr.loadSkill(skillName, destDir_);
        string bodyPreview = body;
        if (body.length > 4096) {
            // TODO: should count grapheme so chinese letters work correctly
            body = body[0 .. 4096] ~ format("\n\n... (truncated, skill is %s bytes total",
                    body.length);
        }
        return ExecuteFuncResult("Skill loaded: " ~ skillName ~ "\n" ~ "Copied to: " ~ destDir ~ "\n\n" ~ bodyPreview,
                success: true);
    } catch (Exception e) {
        return ExecuteFuncResult("error: loading skill '" ~ skillName ~ "': " ~ e.msg,
                success: false);
    }
}
