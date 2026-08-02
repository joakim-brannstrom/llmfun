module llm.skill;

import logger = std.logger;
import std.algorithm : filter, map, among;
import std.array : appender, join, Appender, empty;
import std.conv : to, text;
import std.file : exists, isDir, dirEntries, SpanMode, getSize, readText, rename;
import std.format : format;
import std.path : baseName, buildPath;
import std.range : array;
import std.regex : regex, matchFirst;
import std.string : strip, splitLines, split;
import std.typecons : Nullable, nullable;

import my.file : copyRecurse;
import my.path : AbsolutePath, Path;
import dyaml;

import llm.config : LlmConfig;

SkillManager makeSkillManager(ref LlmConfig llmConf) {
    SkillManager rval;
    if (!llmConf.disableSkills) {
        try {
            rval = new SkillManager();
            rval.discover(llmConf.skillPaths.map!(a => AbsolutePath(a)).array);
        } catch (Exception e) {
            logger.errorf("Skill discovery failed: %s. Continuing without skills.", e.msg);
            rval = new SkillManager();
        }
    } else {
        rval = new SkillManager();
    }
    return rval;
}

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

/// Semantic version struct.
struct SemVer {
    int[] value;

    int major() @safe pure nothrow const @nogc {
        if (value.length >= 1)
            return value[0];
        return 0;
    }

    int minor() @safe pure nothrow const @nogc {
        if (value.length >= 2)
            return value[1];
        return 0;
    }

    int patch() @safe pure nothrow const @nogc {
        if (value.length >= 3)
            return value[2];
        return 0;
    }

    bool valid() @safe pure nothrow const @nogc {
        return !value.empty;
    }
}

/// Parse a semantic version string. Handles X.Y.Z, X.Y, X formats.
/// Strips leading v/V prefix and pre-release suffixes at - or +.
/// Returns invalid SemVer (valid==false) on parse failure with a warning log.
SemVer parseSemVer(string verStr) {
    auto s = verStr.strip;
    if (s.empty) {
        logger.warningf("Failed to parse version string: empty string");
        return SemVer.init;
    }

    if (s[0].among('v', 'V')) {
        s = s[1 .. $];
    }

    // Strip pre-release suffix at - or +
    foreach (i, c; s) {
        if (c == '-' || c == '+') {
            s = s[0 .. i];
            break;
        }
    }

    SemVer rval;
    foreach (p; s.split('.')) {
        try {
            rval.value ~= p.to!int;
        } catch (Exception e) {
            logger.warningf("Failed to parse version string: '%s' part '%s': %s", verStr, p, e.msg);
            return SemVer.init;
        }
    }

    return rval;
}

/// Compare two version strings.
/// Returns -1 (source older), 0 (equal), or +1 (source newer).
/// Edge cases:
///   - Both empty → 0
///   - Source empty, dest not → -1
///   - Source not empty, dest empty → +1
///   - Source unparseable → 0 (skip)
///   - Dest unparseable → +1 (copy)
int compareVersions(string sourceVersion, string destVersion) {
    // Both empty → equal
    if (sourceVersion.empty && destVersion.empty) {
        return 0;
    }

    // Source empty, dest not → source is "older"
    if (sourceVersion.empty) {
        return -1;
    }

    // Source not empty, dest empty → source is "newer"
    if (destVersion.empty) {
        return +1;
    }

    auto src = parseSemVer(sourceVersion);
    auto dst = parseSemVer(destVersion);

    // Source unparseable → skip (return 0)
    if (!src.valid) {
        return 0;
    }

    // Dest unparseable → treat as newer source (return +1)
    if (!dst.valid) {
        return +1;
    }

    // Three-way numeric comparison
    if (src.major != dst.major)
        return src.major > dst.major ? 1 : -1;
    if (src.minor != dst.minor)
        return src.minor > dst.minor ? 1 : -1;
    if (src.patch != dst.patch)
        return src.patch > dst.patch ? 1 : -1;

    return 0;
}

unittest {
    // Basic format: X.Y.Z
    auto v1 = parseSemVer("1.2.3");
    assert(v1.valid);
    assert(v1.major == 1);
    assert(v1.minor == 2);
    assert(v1.patch == 3);

    // Partial format: X.Y
    auto v2 = parseSemVer("1.2");
    assert(v2.valid);
    assert(v2.major == 1);
    assert(v2.minor == 2);
    assert(v2.patch == 0);

    // Partial format: X
    auto v3 = parseSemVer("1");
    assert(v3.valid);
    assert(v3.major == 1);
    assert(v3.minor == 0);
    assert(v3.patch == 0);

    // Leading v prefix
    auto v4 = parseSemVer("v1.0.0");
    assert(v4.valid);
    assert(v4.major == 1);
    assert(v4.minor == 0);
    assert(v4.patch == 0);

    // Leading V prefix
    auto v5 = parseSemVer("V2.3.4");
    assert(v5.valid);
    assert(v5.major == 2);
    assert(v5.minor == 3);
    assert(v5.patch == 4);

    // Pre-release suffix at -
    auto v6 = parseSemVer("1.0.0-beta.1");
    assert(v6.valid);
    assert(v6.major == 1);
    assert(v6.minor == 0);
    assert(v6.patch == 0);

    // Pre-release suffix at +
    auto v7 = parseSemVer("1.0.0+build123");
    assert(v7.valid);
    assert(v7.major == 1);
    assert(v7.minor == 0);
    assert(v7.patch == 0);

    // Extra segments beyond major.minor.patch are ignored
    auto v8 = parseSemVer("1.2.3.4");
    assert(v8.valid);
    assert(v8.major == 1);
    assert(v8.minor == 2);
    assert(v8.patch == 3);

    // Unparseable returns invalid
    auto v9 = parseSemVer("abc");
    assert(!v9.valid);

    // Empty string returns invalid
    auto v10 = parseSemVer("");
    assert(!v10.valid);

    // Lone v prefix returns invalid
    auto v11 = parseSemVer("v");
    assert(!v11.valid);

    // Trailing dot returns invalid
    auto v12 = parseSemVer("1.2.3.");
    assert(!v12.valid);

    // Leading minus treated as pre-release delimiter, returns invalid
    auto v13 = parseSemVer("-1.2.3");
    assert(!v13.valid);
}

unittest {
    // Equal versions
    assert(compareVersions("1.0.0", "1.0.0") == 0);

    // Newer source
    assert(compareVersions("2.0.0", "1.0.0") == 1);
    assert(compareVersions("1.1.0", "1.0.0") == 1);
    assert(compareVersions("1.0.1", "1.0.0") == 1);

    // Older source
    assert(compareVersions("1.0.0", "2.0.0") == -1);
    assert(compareVersions("1.0.0", "1.1.0") == -1);
    assert(compareVersions("1.0.0", "1.0.1") == -1);

    // Both empty
    assert(compareVersions("", "") == 0);

    // Dest empty, source not -> source is "newer"
    assert(compareVersions("1.0.0", "") == +1);

    // Source empty, dest not -> source is "older"
    assert(compareVersions("", "1.0.0") == -1);

    // Source unparseable -> skip (0)
    assert(compareVersions("abc", "1.0.0") == 0);

    // Dest unparseable -> copy (+1)
    assert(compareVersions("1.0.0", "abc") == +1);

    // Both unparseable -> skip (0)
    assert(compareVersions("abc", "xyz") == 0);

    // Leading v prefix stripped
    assert(compareVersions("v1.0.0", "1.0.0") == 0);

    // Pre-release suffix stripped
    assert(compareVersions("1.0.0-beta", "1.0.0") == 0);

    // Partial versions equal
    assert(compareVersions("1.0.0", "1.0") == 0);
    assert(compareVersions("1", "1.0.0") == 0);

    // Extra segments ignored
    assert(compareVersions("1.2.3.4", "1.2.3") == 0);
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
            bool[string] scanned;

            foreach (path; searchPaths.filter!(a => a.exists && a.isDir)) {
                if (path !in scanned) {
                    logger.tracef("Scanning '%s' for skills", path);
                    scanDirectory(path);
                    scanned[path] = true;
                }
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

    Skill[] getAlwaysApplySkills() @safe {
        return skills.values.filter!(s => s.alwaysApply).array;
    }

    /// Load cached skill body by name. Throws SkillNotFoundException if missing.
    string loadSkillBody(string name) {
        if (name !in skills) {
            throw new SkillNotFoundException(i"Skill not found: $(name)".text);
        }
        return skills[name].loadBody();
    }

    /// Copy skill dir to destDir (idempotent). Returns skill body.
    /// Throws: SkillNotFoundException, Exception (empty destDir, wrong skill, malformed SKILL.md).
    string loadSkill(string name, Path destDir, bool overwrite) {
        if (name !in skills) {
            throw new SkillNotFoundException(i"Skill not found: $(name)".text);
        }
        auto skill = skills[name];

        if (destDir.empty || destDir.strip.empty) {
            throw new Exception("destDir must not be empty");
        }

        if (!exists(destDir)) {
            copyRecurse(skill.dir, destDir);
            logger.tracef("Copied skill '%s' to %s", name, destDir);
            return skill.loadBody();
        }

        const existingSkillMd = buildPath(destDir, "SKILL.md");
        if (!exists(existingSkillMd)) {
            throw new Exception(i"Destination '$(destDir)' exists but does not contain a valid SKILL.md. The parameter destDir should be $(
                    destDir)/$(name)".text);
        }

        const existingContent = readText(existingSkillMd);
        const existingFm = parseFrontmatter(existingContent);
        if (!existingFm.parseErrors.empty) {
            throw new Exception(format!"Cannot verify existing skill at '%s': SKILL.md parse error: %s"(destDir,
                    existingFm.parseErrors.join(", ")));
        }
        if (!existingFm.name.empty && existingFm.name != name) {
            throw new Exception(format!"Destination '%s' contains skill '%s', not '%s'"(destDir,
                    existingFm.name, name));
        }

        // Same skill at destination - compare versions
        const cmp = compareVersions(skill.version_, existingFm.version_);
        if (cmp > 0 || overwrite) {
            // Source is newer or overwrite requested - backup and copy
            const isUpgrade = cmp > 0;
            string backupDir = destDir.toString ~ ".llmfun_backup";
            if (isUpgrade) {
                logger.infof("Skill '%s' version %s is newer than %s at %s - upgrading",
                        name, skill.version_, existingFm.version_, destDir);
            } else {
                logger.infof("Skill '%s' overwrite requested at %s - backing up and copying",
                        name, destDir);
            }

            rename(destDir, backupDir);

            logger.tracef("Backed up existing skill '%s' to %s", name, backupDir);
            scope (failure) {
                if (exists(backupDir)) {
                    try {
                        rename(backupDir, destDir);
                        logger.warningf("Restored backup of skill '%s' from %s after copy failure",
                                name, backupDir);
                    } catch (Exception e) {
                        logger.errorf("Failed to restore backup of skill '%s' from %s: %s",
                                name, backupDir, e.msg);
                    }
                }
            }

            copyRecurse(skill.dir, destDir);

            // Verify copy produced a valid SKILL.md
            const copiedSkillMd = buildPath(destDir, "SKILL.md");
            if (!exists(copiedSkillMd)) {
                throw new Exception(
                        format!"Copy completed but SKILL.md missing at '%s' - copy may be incomplete"(
                        destDir));
            }

            if (isUpgrade) {
                logger.infof("Upgraded skill '%s' to version %s at %s", name,
                        skill.version_, destDir);
            } else {
                logger.infof("Overwrote skill '%s' at %s", name, destDir);
            }
        } else {
            logger.tracef("Skill '%s' already present at %s with version %s (source version %s), skipping copy",
                    name, destDir, existingFm.version_, skill.version_);
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
