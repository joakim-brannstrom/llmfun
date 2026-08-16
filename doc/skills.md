# Skills in llmfun

Skills are portable, reusable instruction bundles that extend llmfun with specialized expertise. They follow the [Agent Skills open standard](https://agentskills.io) — the same format used by Claude Code, Cursor, VS Code, GitHub Copilot, and Gemini CLI. A skill written for llmfun works in any tool that implements the standard, and vice versa.

---

## Table of Contents

- [What Is a Skill](#what-is-a-skill)
- [How It Works](#how-it-works)
  - [Progressive Disclosure](#progressive-disclosure)
  - [Startup Flow](#startup-flow)
  - [Runtime Flow](#runtime-flow)
- [Skill Directory Structure](#skill-directory-structure)
- [SKILL.md Format](#skillmd-format)
  - [Frontmatter Fields](#frontmatter-fields)
  - [Markdown Body](#markdown-body)
- [Skill Naming Rules](#skill-naming-rules)
- [Discovery Locations](#discovery-locations)
- [Configuration](#configuration)
- [Creating a Skill](#creating-a-skill)
- [Using Skills](#using-skills)
  - [loadSkill Tool](#loadskill-tool)
  - [Version Checking & Overwrite](#version-checking--overwrite)
  - [alwaysApply Skills](#alwaysapply-skills)
  - [glob-Triggered Skills](#glob-triggered-skills)
- [Sandbox Containment](#sandbox-containment)
- [Error Handling](#error-handling)
- [Limits and Guards](#limits-and-guards)
- [Examples](#examples)
- [Troubleshooting](#troubleshooting)

---

## What Is a Skill

A skill is a **directory** containing a `SKILL.md` entry point file and optional supporting resources. It encapsulates everything an agent needs to complete a specific task: instructions, workflows, scripts, and reference materials.

Think of a skill as an "operations manual" that tells the agent:
- **What** it does (the action)
- **When** to use it (triggering conditions)
- **How** to do it (step-by-step procedures)
- **Which tools** to use (file access, scripts, references)

### Key Characteristics

| Characteristic | Detail |
|---------------|--------|
| **Portable** | Works across llmfun and 30+ other AI agent tools |
| **On-demand** | Full instructions load only when needed |
| **Self-contained** | Includes scripts, references, and assets |
| **Lightweight** | Only name + description loaded at startup |
| **Sandboxed** | Skills enter the agent sandbox via controlled copy. `loadSkill` reject paths outside of the workarea (sandbox). |
| **Bounded** | Skill bodies over 4096 bytes are truncated to fit the context window |

---

## How It Works

### Progressive Disclosure

Skills use a three-phase loading strategy to minimize context window usage:

| Phase | When | What Is Loaded | Token Cost |
|-------|------|----------------|------------|
| **Discovery** | Agent startup | Only `name` and `description` of every skill | Minimal |
| **Activation** | Task matches a skill | Full `SKILL.md` body via `loadSkill` tool | On-demand |
| **Execution** | As needed | Files from `references/`, `scripts/`, `assets/` | On-demand |

At startup, the agent sees a manifest of available skills (name + description only). When the agent decides a task requires a specific skill, it calls the `loadSkill` tool to load the full instructions. This ensures that even with hundreds of installed skills, the startup context overhead remains minimal.

### Startup Flow

```
1. llmfun starts
2. SkillManager scans configured skill directories
3. For each directory with a valid SKILL.md:
   a. Parse YAML frontmatter (extract name, description, etc.)
   b. Validate skill name and required fields
   c. Add to internal registry (first occurrence wins on duplicates)
4. Build <available_skills> XML manifest from registry
5. Inject manifest into agent system prompt
6. Agent is ready — skills available via loadSkill tool
```

Skills are **non-critical**: if discovery fails entirely, llmfun starts normally without skills.

### Runtime Flow

```
1. User: "Can you review my code for bugs?"
2. Agent reads <available_skills> in system prompt
3. Agent matches task → calls loadSkill("code-review", "./.llmfun/loaded_skills/code-review")
4. llmfun copies skill directory into sandbox workarea
5. llmfun returns SKILL.md body as tool result
6. Agent follows skill instructions
7. Agent accesses resources via normal tools (readFile, executeCode)
```

---

## Skill Directory Structure

```
skill-name/
├── SKILL.md              # [Required] Entry point: frontmatter + instructions
├── scripts/              # [Optional] Executable scripts
│   ├── validate.py
│   └── deploy.sh
├── references/           # [Optional] Reference documentation
│   ├── api-schema.md
│   └── style-guide.md
└── assets/               # [Optional] Templates, images, static files
    ├── template.yaml
    └── logo.png
└── examples/             # [Optional] Example files
    ├── config.yaml
    └── template.ts
```

**`SKILL.md`** is the only required file. All subdirectories are optional.

---

## SKILL.md Format

A `SKILL.md` file consists of two parts: YAML frontmatter (between `---` delimiters) followed by a Markdown body.

```markdown
---
name: skill-name
description: What it does and when to use it
---

# Skill Instructions

Detailed instructions for the agent go here...
```

### Frontmatter Fields

#### Required Fields

| Field | Type | Description |
|-------|------|-------------|
| `name` | string | Skill identifier. Must match [naming rules](#skill-naming-rules). |
| `description` | string | What the skill does and when to trigger. The agent decides whether to activate the skill based **solely** on this field. |

#### Optional Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `version` | string | *(empty)* | Semantic version string (e.g., `1.0.0`). |
| `license` | string | *(empty)* | License information. |
| `globs` | string[] | `[]` | File patterns (gitignore-style) for auto-trigger. *Not yet implemented in llmfun.* |
| `alwaysApply` | boolean | `false` | Load skill body on every interaction. Use sparingly. |
| `allowed-tools` | string[] | *(empty)* | Pre-approved tools that skip confirmation. *Not yet implemented in llmfun.* |

### Writing Good Descriptions

The `description` field is critical — the agent decides whether to activate the skill based solely on it. A good description includes:

1. **What the skill does** (the action)
2. **When it should be triggered** (scenarios and keywords)

**Good example:**
```yaml
description: >-
  Extract text and tables from PDF files, fill forms, and merge multiple PDFs.
  Use when working with PDF documents or when the user mentions PDFs, forms,
  or document extraction.
```

**Bad example:**
```yaml
description: Helps with PDFs.
```

**Tips:**
- Use imperative mood ("Use this skill when...")
- Describe user intent, not implementation details
- Include keywords the agent might encounter
- Maximum 1024 characters (recommended)

### Markdown Body

The body contains the instructions the agent reads upon skill activation. It can include:

- **Rules**: Declarative policies (conventions, constraints, decisions)
- **Workflows**: Step-by-step operational procedures
- **Script references**: Pointers to executable code in `scripts/`
- **Reference document links**: Pointers to detailed docs in `references/`

**Design principle:** Keep inline instructions concise. Detailed flows, patterns, examples, and mutable state belong in `references/`, referenced from the body.

**Guidelines:**
- Split `SKILL.md` if it exceeds ~100 lines
- Split when content spans different domains
- Move advanced functionality into separate files in `references/`

---

## Skill Naming Rules

Skill names must conform to the Agent Skills specification:

| Rule | Example |
|------|---------|
| **Length**: 1–64 characters | `a` (1 char), 64-char string |
| **Allowed**: lowercase letters, numbers, hyphens | `code-review`, `api-client` |
| **No uppercase** | `MySkill` ❌ |
| **No underscores** | `my_skill` ❌ |
| **No leading hyphen** | `-pdf` ❌ |
| **No trailing hyphen** | `pdf-` ❌ |
| **No consecutive hyphens** | `pdf--processing` ❌ |
| **No spaces or special characters** | `my skill` ❌, `my.skill` ❌ |
| **No Unicode** | `café` ❌ |

**Directory name must match `name` field.** If the directory name differs, a warning is logged but the skill still loads (the `name` field is authoritative).

---

## Discovery Locations

llmfun discovers skills by scanning configured directories. Each subdirectory containing a valid `SKILL.md` file is treated as a skill.

### Default Search Paths

When no custom configuration is set, llmfun searches:

1. **Project-local**: `llmfun/skills/` (relative to current working directory)
2. **User-level**: `~/.local/share/llmfun/skills/` (XDG data home)

### Priority Order

Skills are discovered in path order. If duplicate skill names exist across paths, the **first occurrence wins** and subsequent duplicates are logged as warnings.

### Adding Claude Code Skills

To import skills from Claude Code, add `~/.claude/skills` to your `skillPathsUser` configuration:

```yaml
skillPathsUser:
  - llmfun/skills
  - ~/.local/share/llmfun/skills
  - ~/.claude/skills
```

---

## Configuration

Skill behavior is controlled through the llmfun YAML configuration file. All skill-related options are optional with sensible defaults.

### Configuration Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `skillPathsUser` | string[] | `[]` | User-level skill directories (searched first). |
| `skillPathsSystem` | string[] | `[llmfun/skills, ~/.local/share/llmfun/skills]` | System skill directories (searched after user paths). |
| `maxAlwaysApplyTokens` | integer | `4000` | Maximum estimated tokens for always-apply skill bodies. Set to 0 for unlimited (not recommended). |
| `disableSkills` | boolean | `false` | Disable all skill functionality when `true`. |

### Example Configuration

```yaml
skillPathsUser:
  - llmfun/skills
  - ~/.local/share/llmfun/skills
  - ~/.claude/skills
maxManifestSkills: 150
maxAlwaysApplyTokens: 3000
disableSkills: false
```

### Configuration File Location

The YAML config is loaded from `.llmfun.yaml` (project-local), `$LLMFUN_DEFAULT_CONFIG` or `~/.config/llmfun/config.yaml` (user-level). Project-local configuration is merged with the user-level configuration such that first is user-level loaded then project-local overwrite some or all configuration options.

---

## Creating a Skill

### Step 1: Create the Directory

Create a directory with a valid skill name (lowercase, hyphens only):

```bash
mkdir -p ~/.local/share/llmfun/skills/my-skill
```

### Step 2: Write SKILL.md

Create `SKILL.md` with YAML frontmatter and Markdown body:

```markdown
---
name: my-skill
description: >-
  Do something useful. Use when the user mentions specific keywords
  or asks for this type of task.
version: 1.0.0
---

# My Skill

## When to Use

Use this skill when the user:
- Asks for X
- Mentions "keyword" or "scenario"
- Wants Y done

## How to Execute

1. Step one
2. Step two
3. Run `scripts/helper.sh` if needed
4. Read `references/guide.md` for details

## Output Format

Describe the expected output format here.
```

### Step 3: Add Supporting Resources (Optional)

```bash
mkdir -p ~/.local/share/llmfun/skills/my-skill/{scripts,references,assets}
```

Place executable scripts in `scripts/`, documentation in `references/`, and static files in `assets/`.

### Step 4: Test

Restart llmfun and run `/skills` to verify your skill appears in the manifest.

---

## Using Skills

### loadSkill Tool

The `loadSkill` tool is the primary way to activate a skill. The agent calls it automatically when a task matches a skill description, or you can request it explicitly.

**Parameters:**
- `skillName`: The skill's `name` field (from frontmatter)
- `destDir`: Destination path inside the sandbox workarea
- `overwrite`: Force overwrite existing skill regardless of version (default: `false`)

**What happens:**
1. The entire skill directory is copied into the sandbox
2. The SKILL.md body (instructions) is returned as the tool result
   Bodies over 4096 bytes are truncated with a notice like:
   `... (truncated, skill is 8192 bytes total)`. The full file is available on disk at the destination path.
3. The agent can then access resources via normal tools:
   - `readFile("./.llmfun/loaded_skills/my-skill/references/guide.md")`
   - `executeCode("./.llmfun/loaded_skills/my-skill/scripts/helper.sh")`

**Version checking:** When the destination already contains the same skill, versions are compared (see [Version Checking & Overwrite](#version-checking--overwrite)). Newer source versions trigger a backup + copy upgrade. Equal or older versions skip the copy. The `overwrite=true` flag forces copying regardless of version.

**Idempotency:** Loading the same skill to the same destination is safe — the second call skips the copy if versions are equal. Loading a different skill to an existing destination fails with a clear error.

### Version Checking & Overwrite

When loading a skill to a destination that already contains the same skill, llmfun compares versions to decide whether to copy.

#### Version Comparison Rules

Versions are read from the `version` field in each skill's YAML frontmatter. Comparison follows semantic versioning (major.minor.patch):

| Scenario | Source Version | Dest Version | Action |
|----------|---------------|--------------|--------|
| Equal versions | `1.0.0` | `1.0.0` | Skip copy |
| Source newer | `1.2.0` | `1.0.0` | Backup + copy (upgrade) |
| Source older | `0.9.0` | `1.0.0` | Skip copy |
| Source has version, dest doesn't | `1.0.0` | *(empty)* | Backup + copy |
| Both missing versions | *(empty)* | *(empty)* | Skip copy |
| Source version unparseable | `abc` | `1.0.0` | Skip copy |
| Dest version unparseable | `1.0.0` | `abc` | Backup + copy |

**Version format:** Supports `X.Y.Z`, `X.Y`, `X`, with optional leading `v`/`V` prefix. Pre-release suffixes (e.g., `-beta.1`, `+build123`) are stripped for comparison.

**Design rationale:** When the destination has no version but the source does, llmfun treats the source as newer — this ensures versioned skills can upgrade unversioned local copies. The asymmetric handling of unparseable versions follows the same principle: unparseable source versions skip (safe default), while unparseable dest versions trigger copy (optimistic upgrade).

#### Overwrite Flag

Set `overwrite: true` to force copying regardless of version comparison:

```
loadSkill(skillName: "code-review", destDir: "./.llmfun/loaded_skills/code-review", overwrite: true)
```

This is useful when:
- You want to refresh a skill's files without changing its version
- You suspect the local copy is corrupted
- You want to test a modified skill before bumping its version
- You want to **downgrade** to a previous version (use with caution — a backup is created automatically)

**Note:** `overwrite=true` does not bypass skill name validation. It only bypasses the version comparison check.

#### Backup & Recovery

Before any overwrite (whether triggered by version upgrade or `overwrite=true`), the existing destination is backed up:

- **Backup location:** `destDir.llmfun_backup` (e.g., `./.llmfun/loaded_skills/code-review.llmfun_backup`). The suffix is appended to the full `destDir` path provided to `loadSkill`.
- **Backup method:** Atomic rename (O(1) on most filesystems)
- **Automatic rollback:** If the copy fails after backup, the backup is automatically restored using D's `scope (failure)` cleanup mechanism

**Manual recovery:** If you need to roll back a skill upgrade manually:

```bash
# Remove the current (newer) skill
rm -rf ./.llmfun/loaded_skills/code-review

# Restore from backup
mv ./.llmfun/loaded_skills/code-review.llmfun_backup ./.llmfun/loaded_skills/code-review
```

#### Version Upgrade Scenarios

**Scenario 1: Automatic upgrade**
```
Installed: code-review v1.0.0
Available: code-review v1.2.0
Action:    Backup v1.0.0 → code-review.llmfun_backup, copy v1.2.0
```

**Scenario 2: No upgrade needed**
```
Installed: code-review v1.2.0
Available: code-review v1.2.0
Action:    Skip copy (already up to date)
```

**Scenario 3: Force refresh**
```
Installed: code-review v1.2.0
Available: code-review v1.2.0
Action:    Backup + copy (overwrite=true forces it)
```

**Scenario 4: Prevent downgrade**
```
Installed: code-review v2.0.0
Available: code-review v1.0.0
Action:    Skip copy (source is older)
```

---

### alwaysApply Skills

Skills with `alwaysApply: true` in their frontmatter are loaded into every interaction automatically. Their full body is prepended to the system prompt at startup.

**Token budget:** The total size of all always-apply skill bodies is capped at `maxAlwaysApplyTokens` (default 4000). If the budget is exceeded, skills are added in discovery order until the limit is reached, and a warning is logged.

**Use sparingly:** alwaysApply increases context cost on every interaction. Reserve it for critical, always-relevant instructions (e.g., coding standards, security policies).

### glob-Triggered Skills

*Not yet implemented.* The Agent Skills standard supports `globs` for auto-triggering skills when the agent accesses matching files. llmfun parses the `globs` field but does not yet implement automatic activation.

---

## Sandbox Containment

llmfun runs the agent inside a **sandboxed workarea**. Skills live **outside** this sandbox (in user-configured directories). The `loadSkill` tool is the **only** mechanism for skills to enter the sandbox.

**How it works:**
1. `loadSkill` copies the entire skill directory into the sandbox workarea
2. The agent accesses resources via sandbox-relative paths
3. No direct paths to external skill directories are ever exposed to the agent

**Why this matters:** This boundary prevents the agent from accessing arbitrary files on the user's system. Skills must be explicitly loaded into the sandbox before their resources can be used.

---

## Error Handling

llmfun handles skill-related errors gracefully:

| Scenario | Behavior |
|----------|----------|
| Skill discovery fails entirely | Log error, continue without skills |
| Directory in `skillPaths` doesn't exist | Skip silently |
| Subdirectory without SKILL.md | Skip silently |
| SKILL.md > 1MB | Log warning, skip skill |
| Missing `name` or `description` | Log warning, skip skill |
| Invalid skill name | Log warning, skip skill |
| Directory name ≠ `name` field | Log warning (non-fatal), still load |
| Duplicate skill name | Log warning, first wins |
| `loadSkill` with unknown name | Return error to agent |
| `loadSkill` destination conflict | Return error with details |
| YAML parse error | Log warning, skip skill |

---

## Limits and Guards

| Guard | Limit | Purpose |
|-------|-------|---------|
| **File size** | 1MB per SKILL.md | Prevent DoS from oversized files |
| **Manifest entries** | 200 (configurable) | Prevent token budget exhaustion |
| **Always-apply tokens** | 4000 (configurable) | Prevent context exhaustion |
| **Name length** | 64 characters | Per Agent Skills specification |

When limits are exceeded, a truncation notice is included in the manifest or a warning is logged. Omitted skills remain available via `loadSkill`.

---

## Examples

### Minimal Skill

```
code-review/
└── SKILL.md
```

**SKILL.md:**
```markdown
---
name: code-review
description: >-
  Review code for style, bugs, and security issues. Use when user asks for
  code review, PR feedback, or quality checks.
---

# Code Review Skill

## When to Use

Use this skill when the user:
- Asks for a code review
- Mentions "PR", "pull request", or "review"
- Wants style feedback or security checks

## How to Review

1. Check for common bugs (null references, off-by-one, resource leaks)
2. Check for security issues (injection, hardcoded secrets, XSS)
3. Check code style and consistency
4. Summarize findings with severity levels

## Output Format

- **Critical**: Must fix before merge
- **Warning**: Should fix, not blocking
- **Suggestion**: Nice to have
```

### Complete Skill with Resources

```
pdf-processor/
├── SKILL.md
├── scripts/
│   ├── extract.py
│   └── validate.py
├── references/
│   ├── form-schema.json
│   └── api-docs.md
└── assets/
    └── template.pdf
```

**SKILL.md:**
```markdown
---
name: pdf-processor
description: >-
  Extract text and tables from PDF files, fill forms, and merge documents.
  Use when working with PDF files or when the user mentions PDFs, forms,
  or document extraction.
version: 1.2.0
---

# PDF Processing Skill

## Quick Start

```bash
python scripts/extract.py input.pdf --output output.json
```

## Workflows

### Extract Text

1. Run `scripts/extract.py <pdf-file> --text`
2. Read `references/api-docs.md` for output format

### Fill Forms

1. Read `references/form-schema.json` for field definitions
2. Run `scripts/validate.py <filled-form>`
3. Merge with base template from `assets/template.pdf`

## References

- API documentation: `references/api-docs.md`
- Form schema: `references/form-schema.json`
```

### Always-Apply Skill

```markdown
---
name: coding-standards
description: Project-wide coding conventions and style guidelines.
alwaysApply: true
---

# Coding Standards

All code must follow these conventions:
- Use 4-space indentation
- Maximum line length: 100 characters
- Function names: camelCase
- Type names: PascalCase
```

---

## Troubleshooting

### Skill Not Appearing in Manifest

1. **Check directory name**: Must match `name` field (warning logged if different)
2. **Check SKILL.md exists**: The file must be named exactly `SKILL.md` (uppercase)
3. **Check required fields**: `name` and `description` must be present in frontmatter
4. **Check name format**: Lowercase, numbers, hyphens only (no uppercase, underscores, spaces)
5. **Check file size**: Must be ≤ 1MB
6. **Check skillPaths**: Verify your skill directory is in a configured search path
7. **Run `/skills`**: Lists all discovered skills and their status

### Skill Loads but Resources Are Missing

1. **Verify directory structure**: `scripts/`, `references/`, `assets/` must be inside the skill directory
2. **Check paths in SKILL.md**: Resource paths should be relative to skill directory
3. **Access via sandbox**: After `loadSkill`, access resources via the sandbox destination path

### Always-Apply Skill Not Loading

1. **Check `alwaysApply: true`**: Must be set in frontmatter (boolean, not string)
2. **Check token budget**: If `maxAlwaysApplyTokens` is exceeded, skill may be omitted
3. **Check logs**: Warning is logged if skill is omitted due to budget

### Skill Not Upgrading

1. **Check version field**: Verify the source skill's `version` in frontmatter is newer than the installed version
2. **Check version format**: Must be parseable as semantic version (e.g., `1.0.0`, `v1.2.0`). Invalid versions like `abc` cause the copy to be skipped
3. **Use `overwrite: true`**: Force a refresh if versions are equal but you suspect file corruption
4. **Check for backup**: Look for `destDir.llmfun_backup` to confirm a previous upgrade occurred
5. **Check logs**: Warnings are logged when version parsing fails

### Recover from Failed Upgrade

If a skill upgrade fails and leaves things in a bad state:

1. **Check for backup**: Look for `destDir.llmfun_backup`
2. **Restore manually**:
   ```bash
   rm -rf ./.llmfun/loaded_skills/my-skill
   mv ./.llmfun/loaded_skills/my-skill.llmfun_backup ./.llmfun/loaded_skills/my-skill
   ```
3. **Retry**: Call `loadSkill` again after restoring

---

### Disable Skills Entirely

Set `"disableSkills": true` in your config. No skills will be discovered or loaded.

---

## Compatibility

llmfun implements the [Agent Skills open standard](https://agentskills.io/specification). Skills are compatible with:

- Claude Code / Claude.ai
- VS Code / GitHub Copilot
- Cursor
- Gemini CLI
- Roo Code
- And other tools implementing the standard

**llmfun-specific extensions:**
- `loadSkill` copies skills into sandbox (required for containment)
- Version checking with automatic backup + upgrade on newer versions
- `overwrite` flag to force skill refresh regardless of version
- `maxManifestSkills` and `maxAlwaysApplyTokens` configuration options
- `/skills` slash command for listing skills

**Not yet implemented:**
- `globs` auto-trigger (planned)
- `allowed-tools` permission bypass (planned)
