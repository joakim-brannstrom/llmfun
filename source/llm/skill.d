module llm.skill;

import logger = std.logger;
import std.algorithm : filter, map;
import std.array : appender, join, Appender, empty;
import std.file : exists, isDir, dirEntries, SpanMode, getSize, readText;
import std.format : format;
import std.path : baseName, buildPath;
import std.range : array;
import std.string : strip, splitLines;
import std.regex : regex, matchFirst;
import std.typecons : Nullable, nullable;

import my.file : copyRecurse;
import my.path : AbsolutePath, Path;

import dyaml;

struct Skill {
    string name;
    string description;
    string[] globs;
    bool alwaysApply;
    string version_;
    string license;
    string[] allowedTools;
    AbsolutePath dir;
    AbsolutePath skillMdPath;

    private string _cachedBody;

    /// Cached skill body (post-frontmatter content).
    string loadBody() {
        if (!_cachedBody.empty) {
            return _cachedBody;
        }
        _cachedBody = skillMdPath.readText.stripFrontmatter;
        return _cachedBody;
    }

    /// Uncached disk read. For debugging/reloading.
    string loadFullContent() const {
        return skillMdPath.readText;
    }
}

struct SkillResult {
    Skill skill;
    string error; // empty = success, non-empty = failure description
}

/// YAML frontmatter from SKILL.md.
struct SkillFrontmatter {
    string name;
    string description;
    string[] globs;
    bool alwaysApply; // defaults to false
    string version_;
    string license;
    string[] allowedTools;
    string[] parseErrors;
}

/// Thrown when a skill is not found.
class SkillNotFoundException : Exception {
    this(string msg, string file = __FILE__, size_t line = __LINE__) {
        super(msg, file, line);
    }
}

/// Extract YAML between `---` delimiters. Opening `---` must be first line.
/// Returns "" if no frontmatter. Stops at first `---` line in YAML content.
string extractYamlFrontMatter(string content) {
    auto lines = content.splitLines;

    if (lines.empty || lines[0].strip != "---") {
        return "";
    }

    string[] yamlLines;
    foreach (line; lines[1 .. $]) {
        auto trimmed = line.strip;
        if (trimmed == "---") {
            return yamlLines.join("\n");
        }
        yamlLines ~= line;
    }

    return "";
}

/// Parse YAML frontmatter. Populates parseErrors on failure (logged at warning).
SkillFrontmatter parseFrontmatter(string content) {
    auto fm = SkillFrontmatter();

    auto yamlBlock = extractYamlFrontMatter(content);
    if (yamlBlock.empty) {
        return fm;
    }

    try {
        auto node = Loader.fromString(yamlBlock).load();

        if (node.containsKey("name"))
            fm.name = node["name"].as!string;

        if (node.containsKey("description"))
            fm.description = node["description"].as!string;

        if (node.containsKey("alwaysApply"))
            fm.alwaysApply = node["alwaysApply"].as!bool;

        if (node.containsKey("version"))
            fm.version_ = node["version"].as!string;

        if (node.containsKey("license"))
            fm.license = node["license"].as!string;

        if (node.containsKey("globs")) {
            foreach (string item; node["globs"])
                fm.globs ~= item;
        }

        if (node.containsKey("allowed-tools")) {
            foreach (string item; node["allowed-tools"])
                fm.allowedTools ~= item;
        }
    } catch (Exception e) {
        logger.warningf("Failed to parse skill frontmatter: %s", e.msg);
        fm.parseErrors ~= e.msg;
    }

    return fm;
}

/// Remove YAML frontmatter, returning trimmed body. Stops at first `---` in YAML.
string stripFrontmatter(string content) {
    auto lines = content.splitLines;
    bool foundOpening = false;
    bool foundClosing = false;
    size_t startIdx = 0;

    foreach (i, line; lines) {
        auto trimmed = line.strip;
        if (!foundOpening) {
            if (trimmed == "---") {
                foundOpening = true;
            }
        } else if (!foundClosing) {
            if (trimmed == "---") {
                foundClosing = true;
                startIdx = i + 1;
            }
        }
    }

    if (!foundOpening || !foundClosing) {
        return content.strip;
    }

    auto bodyLines = lines[startIdx .. $];
    return bodyLines.join("\n").strip;
}

/// Validate skill name: 1-64 lowercase alphanumeric chars with hyphens (no leading/trailing/consecutive).
bool isValidName(string name) {
    if (name.length > 64) {
        return false;
    }

    static auto namePattern = regex(r"^[a-z0-9]+(-[a-z0-9]+)*$");
    return cast(bool) matchFirst(name, namePattern);
}

/// Check if directory base name matches skill name. Uses `.toString` because `AbsolutePath` lacks `.baseName`.
bool dirNameMatches(AbsolutePath dir, string name) {
    auto dirBaseName = dir.toString.baseName;
    return dirBaseName == name;
}

string xmlEscape(string s) {
    auto ap = appender!string();
    foreach (c; s) {
        switch (c) {
        case '&':
            ap.put("&amp;");
            break;
        case '<':
            ap.put("&lt;");
            break;
        case '>':
            ap.put("&gt;");
            break;
        case '"':
            ap.put("&quot;");
            break;
        case '\'':
            ap.put("&apos;");
            break;
        default:
            ap.put(c);
            break;
        }
    }
    return ap[];
}

class SkillManager {
    private {
        Skill[string] skills; // name -> Skill, first wins
        string manifestXmlCache;
        size_t skippedCount;
    }

    /// Discover skills from search paths. Clears previous skills (idempotent).
    void discover(AbsolutePath[] searchPaths) {
        skills = null;
        manifestXmlCache = null;
        skippedCount = 0;

        try {
            foreach (path; searchPaths.filter!(a => a.exists && a.isDir)) {
                logger.tracef("Scanning '%s' for skills", path);
                scanDirectory(path);
            }

            foreach (path; searchPaths.filter!(a => !a.exists || !a.isDir)) {
                logger.tracef(
                        "Unable to read skills from '%s' because either the path do not exist or it isn't a directory",
                        path);
            }
        } catch (Exception e) {
            logger.errorf("Error during skill discovery: %s", e.msg);
            // Non-critical; continue without skills
            return;
        }

        auto alwaysApplyCount = skills.values.filter!(s => s.alwaysApply).array.length;
        logger.infof("Discovered %d skills (%d always-apply, %d skipped)",
                skills.length, alwaysApplyCount, skippedCount);
    }

    private void scanDirectory(Path path) {
        try {
            foreach (entry; dirEntries(path.toString, SpanMode.shallow)) {
                if (!entry.isDir) {
                    continue;
                }

                auto skillMdPath = AbsolutePath(buildPath(entry.name, "SKILL.md"));
                if (!exists(skillMdPath.toString)) {
                    continue;
                }

                auto result = parseSkillMd(skillMdPath);
                if (!result.error.empty) {
                    logger.warningf("Skipping skill at %s: %s", skillMdPath, result.error);
                    skippedCount++;
                    continue;
                }

                if (result.skill.name in skills) {
                    logger.warningf("Duplicate skill name '%s' at %s (first occurrence at %s wins)",
                            result.skill.name, skillMdPath, skills[result.skill.name].skillMdPath);
                    skippedCount++;
                    continue;
                }

                skills[result.skill.name] = result.skill;
            }
        } catch (Exception e) {
            logger.warningf("Error scanning directory %s: %s", path, e.msg);
        }
    }

    private SkillResult parseSkillMd(AbsolutePath skillMdPath) {
        enum size_t maxSize = 1_048_576; // 1MB
        auto fileSize = getSize(skillMdPath.toString);
        if (fileSize > maxSize) {
            return SkillResult(Skill.init,
                    format!"SKILL.md too large: %s bytes (max %s)"(fileSize, maxSize));
        }

        auto content = readText(skillMdPath.toString);
        auto fm = parseFrontmatter(content);

        if (fm.name.empty) {
            return SkillResult(Skill.init, "missing required 'name' field");
        }
        if (fm.description.empty) {
            return SkillResult(Skill.init, "missing required 'description' field");
        }

        if (!isValidName(fm.name)) {
            return SkillResult(Skill.init, format!"invalid skill name '%s': must be 1-64 lowercase alphanumeric chars with hyphens (no leading/trailing/consecutive hyphens)"(
                    fm.name));
        }

        // Non-fatal warning only
        auto parentDir = skillMdPath.dirName;
        auto dirBaseName = parentDir.toString.baseName;
        if (dirBaseName != fm.name) {
            logger.warningf("Skill directory name '%s' does not match skill name '%s' (at %s)",
                    dirBaseName, fm.name, skillMdPath);
        }

        auto skill = Skill();
        skill.name = fm.name;
        skill.description = fm.description;
        skill.globs = fm.globs;
        skill.alwaysApply = fm.alwaysApply;
        skill.version_ = fm.version_;
        skill.license = fm.license;
        skill.allowedTools = fm.allowedTools;
        skill.dir = parentDir;
        skill.skillMdPath = skillMdPath;

        return SkillResult(skill, "");
    }

    /// Discovered skills. Order not guaranteed.
    Skill[] getManifest() {
        return skills.values.array;
    }

    Skill[] getAlwaysApplySkills() {
        return skills.values.filter!(s => s.alwaysApply).array;
    }

    /// Load cached skill body by name. Throws SkillNotFoundException if missing.
    string loadSkillBody(string name) {
        if (name !in skills) {
            throw new SkillNotFoundException(format!"Skill not found: %s"(name));
        }
        return skills[name].loadBody();
    }

    /// Copy skill dir to destDir (idempotent). Returns skill body.
    /// Throws: SkillNotFoundException, Exception (empty destDir, wrong skill, malformed SKILL.md).
    string loadSkill(string name, string destDir) {
        if (name !in skills) {
            throw new SkillNotFoundException(format!"Skill not found: %s"(name));
        }
        auto skill = skills[name];

        if (destDir.empty || destDir.strip.empty) {
            throw new Exception("destDir must not be empty");
        }

        if (exists(destDir)) {
            auto existingSkillMd = buildPath(destDir, "SKILL.md");
            if (exists(existingSkillMd)) {
                auto existingContent = readText(existingSkillMd);
                auto existingFm = parseFrontmatter(existingContent);
                if (!existingFm.parseErrors.empty) {
                    throw new Exception(format!"Cannot verify existing skill at '%s': SKILL.md parse error: %s"(
                            destDir, existingFm.parseErrors.join(", ")));
                }
                if (!existingFm.name.empty && existingFm.name != name) {
                    throw new Exception(format!"Destination '%s' contains skill '%s', not '%s'"(destDir,
                            existingFm.name, name));
                }
                logger.tracef("Skill '%s' already present at %s, skipping copy", name, destDir);
            } else {
                throw new Exception(
                        format!"Destination '%s' exists but does not contain a valid SKILL.md"(
                        destDir));
            }
        } else {
            copyRecurse(skill.dir, destDir.Path);
            logger.tracef("Copied skill '%s' to %s", name, destDir);
        }

        return skill.loadBody();
    }

    /// Cached XML manifest. maxEntries=0 means unlimited. Adds truncation notice if clipped.
    string getManifestXml(long maxEntries) {
        if (!manifestXmlCache.empty) {
            return manifestXmlCache;
        }

        auto allSkills = skills.values.array;
        long count;
        auto skillsToInclude = allSkills;
        if (maxEntries > 0 && allSkills.length > cast(size_t) maxEntries) {
            skillsToInclude = allSkills[0 .. cast(size_t) maxEntries];
            count = cast(long) maxEntries;
        } else {
            count = cast(long) allSkills.length;
        }

        auto ap = appender!string();
        ap.put("<available_skills>\n");
        ap.put("Use the loadSkill tool to load the full instructions for any skill\n");
        ap.put("when its description matches the current task.\n");
        ap.put("\n");
        foreach (skill; skillsToInclude) {
            ap.put("  <skill alwaysApply=\"");
            ap.put(skill.alwaysApply ? "true" : "false");
            ap.put("\">\n");
            ap.put("    <name>");
            ap.put(xmlEscape(skill.name));
            ap.put("</name>\n");
            ap.put("    <description>");
            ap.put(xmlEscape(skill.description));
            ap.put("</description>\n");
            ap.put("  </skill>\n");
        }
        ap.put("</available_skills>\n");

        if (maxEntries > 0 && allSkills.length > cast(size_t) maxEntries) {
            long remaining = cast(long) allSkills.length - maxEntries;
            ap.put("<!-- ");
            ap.put(format!"%s more skills available. Use loadSkill with the skill name to see them."(
                    remaining));
            ap.put(" -->\n");
        }

        auto result = ap[];
        manifestXmlCache = result;
        return result;
    }
}

/// Markdown block of always-apply skill bodies. Token budget via ApproxTokenSize.
/// maxTokens<=0 means unlimited (warned once). Appends warning if skills omitted.
string buildAlwaysApplyBlock(Skill[] alwaysApply, long maxTokens) {
    import llm.config : ApproxTokenSize;

    if (alwaysApply.empty) {
        return null;
    }

    bool unlimited = maxTokens <= 0;
    static bool unlimitedWarned;
    if (unlimited && !unlimitedWarned) {
        unlimitedWarned = true;
        logger.warningf("maxAlwaysApplyTokens is %d, treating as unlimited. Consider setting a positive value to prevent context exhaustion.",
                maxTokens);
    }

    auto ap = appender!string();
    long totalTokens = 0;
    size_t omittedCount = 0;

    foreach (skill; alwaysApply) {
        auto body = skill.loadBody();
        auto block = format("## Skill: %s\n%s\n\n", skill.name, body);
        auto estimatedTokens = cast(long)(block.length / ApproxTokenSize);

        if (!unlimited && totalTokens + estimatedTokens > maxTokens) {
            omittedCount++;
            logger.warningf("Omitting always-apply skill '%s' due to token budget (limit: %d tokens, would add ~%d)",
                    skill.name, maxTokens, estimatedTokens);
            continue;
        }

        ap.put(block);
        if (!unlimited) {
            totalTokens += estimatedTokens;
        }
    }

    if (omittedCount > 0) {
        ap.put("[Warning: ");
        ap.put(format!"%s always-apply skills omitted due to token budget (limit: %d tokens). Remaining skills are still available via loadSkill."(
                omittedCount, maxTokens));
        ap.put("]\n");
    }

    return ap[];
}
