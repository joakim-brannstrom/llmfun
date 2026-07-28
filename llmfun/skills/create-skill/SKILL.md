---
name: create-skill
description: >-
  Create a new Agent Skill (SKILL.md + supporting files) from scratch. Use when
  the user asks to create, write, generate, or scaffold a new skill for llmfun
  or any Agent Skills-compatible tool. Triggers on: create skill, new skill,
  scaffold skill, generate skill, skill creation, make skill, add skill.
version: 1.1.0
---

# Create Skill

Create a valid, well-structured Agent Skill following the
[Agent Skills open standard](https://agentskills.io).

## When to Use

Use this skill when the user:
- Asks to create a new skill
- Wants to scaffold a skill directory
- Needs help writing a SKILL.md file
- Wants to convert a workflow or thinking template into a reusable skill

## Skill Anatomy

```
skill-name/
├── SKILL.md              # [Required] Frontmatter + Markdown instructions
├── scripts/              # [Optional] Executable scripts
├── references/           # [Optional] Reference documentation
├── assets/               # [Optional] Templates, static files
└── examples/             # [Optional] Example files
```

## Step-by-Step Process

### 1. Gather Requirements
- What does the skill do? When should it trigger?
- Which subdirectories are needed? (`scripts/`, `references/`, `assets/`)
- Does it relate to other skills? (pipeline relationships)
- Should it be `alwaysApply`? (only if truly needed on every interaction)

### 2. Determine the Name
- Allowed: lowercase letters, numbers, hyphens only
- Directory name must match `name` field exactly
- Use prefixes for families: `code-task`, `code-review`
- Use `llmfun-` prefix for internal functionality

### 3. Write the Description (most critical field)
- Include **what** it does and **when** to trigger
- End with `Triggers on: keyword1, keyword2, ...`
- Maximum 1024 characters

### 4. Write SKILL.md
- Frontmatter: `name`, `description`, `version` (required)
- Body sections in this order: When to Use → Core Principle (optional) → Rules → Workflow → Output Format → References
- Keep body under **4096 bytes** — move details to `references/`
- Hard limit: bodies over 4096 bytes are **truncated on load**

### 5. Create Supporting Files
- `references/workflow.md` — detailed step-by-step protocol
- `references/output-format.md` — output template with example
- `references/decision-rules.md` — concrete patterns and criteria

### 6. Validate
- Name matches directory, body under 4KB, no duplicate content
- Place in `llmfun/skills/` (project-local) or `~/.local/share/llmfun/skills/`

## Rules

- **SKILL.md body must be under 4096 bytes** — critical instructions first, details in references
- **Description must include trigger keywords** — or the agent won't activate
- **Name must match directory** — or the skill won't load
- **No uppercase or underscores** in names — invalid for skill loading
- **Each piece of info in exactly one place** — no duplicate between SKILL.md and references

## Workflow

See `references/workflow.md` for detailed steps.

1. **Determine name** — validate against naming rules
2. **Create directory** and write SKILL.md with `writeFile`
3. **Create subdirectories** and supporting files
4. **Verify** — re-read SKILL.md, check byte size, check for errors
5. **Report** — summarize what was created with full path

## Converting Thinking Templates

See `references/convert-template.md` for a detailed guide.

1. Get the template with `getThinkingTemplate("name")`
2. Map phases → Workflow, rules/principles → Rules, output format → references/
3. Add frontmatter and trigger keywords derived from the template

## References

- Detailed creation workflow: `references/workflow.md`
- Thinking template conversion guide: `references/convert-template.md`
- Common pitfalls: `references/pitfalls.md`
