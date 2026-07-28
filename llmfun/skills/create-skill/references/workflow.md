# Create Skill — Detailed Workflow

## Step 1: Determine Skill Name

Validate the name against naming rules:
- **Allowed**: Lowercase letters (`a-z`), numbers (`0-9`), hyphens (`-`)
- **Forbidden**: Uppercase, underscores, spaces, special characters, Unicode
- **No leading/trailing hyphen**: `-pdf` or `pdf-` are invalid
- **No consecutive hyphens**: `pdf--processing` is invalid
- **Length**: 1–64 characters
- **Directory name must match `name` field**

**Good names:** `code-review`, `pdf-processor`, `deploy`, `llmfun-memory`
**Bad names:** `Code-Review` (uppercase), `code_review` (underscore), `my skill` (space)

**Naming tips:**
- Use a descriptive prefix for related skill families: `code-task`, `code-review`, `code-refactor`
- For llmfun-internal functionality, use `llmfun-` prefix: `llmfun-memory`
- Avoid generic names like `memory` or `plan` that could conflict with external concepts

## Step 2: Create Directory and Write SKILL.md

Use `writeFile` to create `llmfun/skills/skill-name/SKILL.md`. This automatically
creates parent directories.

### Frontmatter

Required fields: `name`, `description`

```yaml
---
name: skill-name
description: >-
  What the skill does and when to use it. Include keywords and trigger scenarios.
  Triggers on: keyword1, keyword2, keyword3.
version: 1.0.0
---
```

Optional frontmatter fields:

| Field | Type | Default | When to Use |
|-------|------|---------|-------------|
| `version` | string | *(empty)* | Track skill evolution with semver |
| `license` | string | *(empty)* | Add licensing information |
| `alwaysApply` | boolean | `false` | Load on every interaction (use sparingly) |
| `globs` | string[] | `[]` | Auto-trigger on file patterns (not yet implemented) |
| `allowed-tools` | string[] | *(empty)* | Pre-approve tools (not yet implemented) |

### Body Sections (recommended order)

```markdown
# Skill Title

## When to Use

List specific scenarios. Be precise about triggers.

## Core Principle (optional)

One-sentence philosophy. Use for skills with a strong conceptual foundation
(e.g., debugging, refactoring, system-design).

## Rules

Declarative policies and constraints. 4-6 rules is a good count.

## Workflow

Numbered list of phases/steps. Reference detailed files for depth.

## Output Format (optional)

Describe expected output, or reference an output-format.md file.

## References

List all supporting files in `references/`.
```

### Body Size Constraints

- **Hard limit**: Bodies over 4096 bytes are truncated on load.
- Put critical instructions (When to Use, Rules, Workflow) first.
- Move detailed content to `references/` files.
- Split SKILL.md if it exceeds ~100 lines.

## Step 3: Create Subdirectories and Supporting Files

Create supporting files using `writeFile`:

### Most Common File: `references/workflow.md`

Contains the detailed step-by-step protocol with all sub-steps and edge cases.

### Common File: `references/output-format.md`

Contains the output template with an example:

```markdown
# Skill Name Output Format

[Description of output format]

```markdown
## [Section]

### [Detail]
- Item 1
- Item 2

## Verification
- [ ] Check A
- [ ] Check B
```

### Common File: `references/decision-rules.md`

Contains concrete patterns, examples, and criteria tables.

### Other Files
- `scripts/` — Executable scripts (Python, shell, etc.)
- `assets/` — Templates, static files

## Step 4: Verify

1. Name in frontmatter matches directory name
2. `name` and `description` are present in frontmatter
3. SKILL.md body is under 4096 bytes
4. No duplicate content between SKILL.md and references
5. All referenced files actually exist
6. Trigger keywords in description match actual use cases

## Step 5: Report

Summarize what was created:

```markdown
Created the `skill-name` skill.

**Skill location:** `llmfun/skills/skill-name/`

**Structure:**
```
skill-name/
├── SKILL.md                      (N bytes — main instructions)
└── references/
    ├── workflow.md               (N bytes — detailed protocol)
    └── output-format.md          (N bytes — output template)
```

**To use:** Restart llmfun, run `/skills` to verify it appears, then trigger
it with a relevant task.
```

## Testing

After creating a skill, tell the user to:
1. Restart llmfun
2. Run `/skills` to verify the skill appears in the manifest
3. Test by triggering the skill with a relevant task
