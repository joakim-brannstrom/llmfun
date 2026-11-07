module llm.tool_call.skill;

import logger = std.logger;
import std.array : empty;
import std.conv : text;
import std.file : exists;
import std.string : strip;

import my.path : AbsolutePath;

import llm.skill;
import llm.config : ToolLimits, PathCategory;
import llm.tool_call.utility;
import llm.tool_call;

mixin RegisterLlmFunctions!();

interface SkillContext : Context {
    SkillManager getSkillManager();
    AbsolutePath workArea();
    bool isPathInsideWorkArea(AbsolutePath path);
    ToolLimits getToolLimits();
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

private pure string loadSkillSuccessMessage(string skillName, string version_,
        string destDir, string body) {
    const versionToken = version_.empty ? "unknown" : version_;
    return i"Skill loaded: $(skillName) (version $(versionToken))\nCopied to: $(destDir)\n\n$(body)"
        .text;
}

private enum fallbackDegradationNote = "references/, scripts/ and assets/ are not available in fallback mode:\n"
    ~ "the skill was loaded from its source directory without copying.";

private pure string loadSkillFallbackMessage(string skillName, string destDir, string body) {
    return i"Skill loaded: $(skillName)\nFailed to copy skill to: $(destDir)\n\n$(
            fallbackDegradationNote)\n\n$(body)".text;
}

@("loadSkill success message first line carries the skill version")
unittest {
    import std.algorithm : canFind, endsWith, startsWith;

    // Versioned skill: the first line carries the frontmatter version.
    auto versioned = loadSkillSuccessMessage("code-task", "1.2.3", "skills/code-task", "body");
    assert(versioned.startsWith("Skill loaded: code-task (version 1.2.3)\n"));
    assert(versioned.canFind("Copied to: skills/code-task"));
    assert(versioned.endsWith("body"));

    // Versionless skill: stable "(version unknown)" token instead of omission.
    auto versionless = loadSkillSuccessMessage("code-task", "", "skills/code-task", "body");
    assert(versionless.startsWith("Skill loaded: code-task (version unknown)\n"));
    assert(versionless.canFind("Copied to: skills/code-task"));
    assert(versionless.endsWith("body"));
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
            if (mgr is null) {
                return ExecuteFuncResult("error: skill manager not available", success: false);
            }
            auto body = mgr.loadSkillBody(params.skillName);
            return ExecuteFuncResult(loadSkillFallbackMessage(params.skillName,
                    params.destDir, body), success: true);
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

    auto destDir_ = pathToWorkarea(ctx, params.destDir, PathCategory.write);
    if (!destDir_.valid) {
        if (destDir_.deniedByFilter) {
            return fallbackLoad();
        }
        return ExecuteFuncResult(destDir_.errorMsg, success: false);
    }

    auto mgr = ctx.getSkillManager();
    if (mgr is null) {
        return ExecuteFuncResult("error: skill manager not available", success: false);
    }

    string body;
    try {
        body = mgr.loadSkill(params.skillName, destDir_, params.overwrite);
    } catch (Exception e) {
        logger.trace(e.msg);
        return fallbackLoad();
    }

    // Fetched outside the try/catch above so a skillVersion failure would
    // surface as a loud error instead of a masked fallback "success" after the
    // copy has already succeeded. It cannot throw today: loadSkill throws
    // SkillNotFoundException unless the name is in skills.
    const version_ = mgr.skillVersion(params.skillName);
    if (body.length > 4096) {
        // TODO: should count grapheme so chinese letters work correctly
        body = i"$(body[0 .. 4096])\n\n... (truncated, skill is $(body.length) bytes total)".text;
    }
    return ExecuteFuncResult(loadSkillSuccessMessage(params.skillName,
            version_, params.destDir, body), success: true);
}

version (unittest) {
    import std.algorithm : canFind, startsWith;
    import std.file : dirEntries, mkdirRecurse, symlink, write, SpanMode;
    import std.path : buildPath;

    import llm.config : FilesystemLimits, buildFilesystemFilterRuntime;
    import llm.skill : SkillManager;
    import llm.test_util : testArea;

    class TestSkillContext : SkillContext {
        AbsolutePath workAreaDir;
        ToolLimits limits_;
        SkillManager mgr_;

        this(string dir) {
            workAreaDir = dir.AbsolutePath;
            limits_ = ToolLimits();
            mkdirRecurse(workAreaDir.toString);
        }

        void setFilesystemFilter(ToolLimits limits) {
            limits_ = limits;
        }

        override bool isPathInsideWorkArea(AbsolutePath path) {
            auto p = path.toString;
            auto w = workAreaDir.toString;
            return p.startsWith(w) && (p.length == w.length || p[w.length] == '/');
        }

        override AbsolutePath workArea() {
            return workAreaDir;
        }

        override ToolLimits getToolLimits() {
            return limits_;
        }

        override SkillManager getSkillManager() {
            return mgr_;
        }
    }

    SkillManager makeSkillManager(string sourceRoot) {
        const skillDir = buildPath(sourceRoot, "test-skill");
        mkdirRecurse(skillDir);
        write(buildPath(skillDir, "SKILL.md"), "---\nname: test-skill\ndescription: a test skill for loadSkill fallback tests\nversion: 1.0.0\n---\n\nbody of the test skill\n");
        auto mgr = new SkillManager();
        mgr.discover([sourceRoot.AbsolutePath]);
        return mgr;
    }
}

@("loadSkill fallback message carries the degradation note between the failure line and the body")
unittest {
    const msg = loadSkillFallbackMessage("test-skill", "skills/dest", "body");
    const expected = "Skill loaded: test-skill\n" ~ "Failed to copy skill to: skills/dest\n" ~ "\n"
        ~ "references/, scripts/ and assets/ are not available in fallback mode:\n"
        ~ "the skill was loaded from its source directory without copying.\n" ~ "\n" ~ "body";
    assert(msg == expected, msg);
}

@("loadSkill with a filter-denied destDir falls back to a body-only load and copies nothing")
unittest {
    auto area = testArea("skill_filter_denied");
    scope (exit)
        area.cleanup();

    auto ctx = new TestSkillContext(buildPath(area.toString, "workarea"));
    ctx.mgr_ = makeSkillManager(buildPath(area.toString, "skill_src"));

    ToolLimits tl;
    tl.filesystem.write = FilesystemLimits.CategoryFilter([], [".*"]);
    tl.filesystemFilterRuntime = buildFilesystemFilterRuntime(tl);
    ctx.setFilesystemFilter(tl);

    auto r = loadSkill(ctx, LoadSkillParams("test-skill", "skills/dest", false));
    assert(r.success, r.msg);
    assert(r.msg == "Skill loaded: test-skill\n" ~ "Failed to copy skill to: skills/dest\n" ~ "\n"
            ~ "references/, scripts/ and assets/ are not available in fallback mode:\n"
            ~ "the skill was loaded from its source directory without copying.\n" ~ "\n" ~ "body of the test skill",
            r.msg);
    assert(dirEntries(ctx.workAreaDir.toString, SpanMode.shallow).empty,
            "a filter-denied loadSkill must not create anything in the workarea");
}

@("loadSkill with a symlinked destDir keeps the hard symlink error, no fallback")
unittest {
    auto area = testArea("skill_symlink_dest");
    scope (exit)
        area.cleanup();

    auto ctx = new TestSkillContext(buildPath(area.toString, "workarea"));
    ctx.mgr_ = makeSkillManager(buildPath(area.toString, "skill_src"));

    const target = buildPath(area.toString, "target");
    mkdirRecurse(target);
    symlink(target, buildPath(ctx.workAreaDir.toString, "dest"));

    auto r = loadSkill(ctx, LoadSkillParams("test-skill", "dest", false));
    assert(!r.success, r.msg);
    assert(r.msg.canFind("is a symlink"), r.msg);
    assert(!r.msg.canFind("not available in fallback mode"), r.msg);
}

@("loadSkill with an absolute destDir keeps the absolute-path error, no fallback")
unittest {
    auto area = testArea("skill_absolute_dest");
    scope (exit)
        area.cleanup();

    auto ctx = new TestSkillContext(buildPath(area.toString, "workarea"));
    ctx.mgr_ = makeSkillManager(buildPath(area.toString, "skill_src"));

    auto r = loadSkill(ctx, LoadSkillParams("test-skill", "/tmp/llmfun_skill_abs_test", false));
    assert(!r.success, r.msg);
    assert(r.msg.canFind("is an absolute path"), r.msg);
    assert(!r.msg.canFind("not available in fallback mode"), r.msg);
}

@(
        "loadSkill with a containment failure keeps the containment error even with a filter configured, no fallback")
unittest {
    auto area = testArea("skill_containment_dest");
    scope (exit)
        area.cleanup();

    auto ctx = new TestSkillContext(buildPath(area.toString, "workarea"));
    ctx.mgr_ = makeSkillManager(buildPath(area.toString, "skill_src"));

    ToolLimits tl;
    tl.filesystem.write = FilesystemLimits.CategoryFilter([], [".*"]);
    tl.filesystemFilterRuntime = buildFilesystemFilterRuntime(tl);
    ctx.setFilesystemFilter(tl);

    auto r = loadSkill(ctx, LoadSkillParams("test-skill", "../outside", false));
    assert(!r.success, r.msg);
    assert(r.msg == "error: path '../outside' must be inside the allowed workarea", r.msg);
    assert(!r.msg.canFind("not available in fallback mode"), r.msg);
}
