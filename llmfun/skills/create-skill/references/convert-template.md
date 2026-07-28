# Converting a Thinking Template to a Skill

Use this guide when converting an existing thinking template into a reusable skill.

## Process

### 1. Get the Template

Call `getThinkingTemplate("template_name")` to load the full content.

### 2. Analyze the Structure

Identify these components in the template:
- **Phases/Steps**: Numbered or sequential steps (e.g., "1. Analyze", "2. Plan")
- **Rules/Principles**: "Important" callouts, core principles, general guidelines
- **Output Format**: The template for reporting results
- **Decision Criteria**: Tables, conditions, or branching logic
- **Edge Cases**: Special handling for unusual situations

### 3. Map to Skill Sections

| Template Component | Skill Section |
|---|---|
| Phases/steps | Workflow (numbered list) |
| Rules/principles | Rules section |
| Core philosophy | Core Principle (optional section) |
| Output format | `references/output-format.md` |
| Decision criteria | `references/decision-rules.md` |
| Detailed sub-steps | `references/workflow.md` |
| When to use | When to Use section |

### 4. Write the Frontmatter

```yaml
---
name: skill-name
description: >-
  [What the skill does]. Use when [triggers]. Triggers on: keyword1, keyword2.
version: 1.0.0
---
```

Derive trigger keywords from the template's description and usage notes.

### 5. Split Depth

**Keep in SKILL.md:**
- When to Use (specific trigger scenarios)
- Core Principle (if the template has one)
- Rules (concise policies)
- Workflow summary (numbered list of phases)
- Output Format summary (or reference to output-format.md)
- References section

**Move to `references/workflow.md`:**
- Detailed sub-steps for each phase
- Edge cases and special handling
- Scenarios (A, B, C, D style branching)

**Move to `references/output-format.md`:**
- Full output template with example
- Formatting rules

### 6. Add Pipeline References

If the template relates to other skills, mention the pipeline:

```
system-design → implementation-plan → code-task
```

This helps the agent understand the skill's place in the workflow.

### 7. Cross-Reference Related Skills

If the template references other tools, patterns, or workflows that exist as
skills, mention them in the References section or Rules section.

## Example Mapping

| Template | Skill Name | Key Changes |
|---|---|---|
| `code_task` | `code-task` | Phases → Workflow, principles → Rules |
| `debugging` | `debugging` | Same structure, added tool table |
| `refactoring` | `code-refactor` | Refactoring types → table, output format → references |
| `system_design` | `system-design` | Added pipeline to implementation-plan, code-task |
| `update_memory` | `llmfun-memory` | Mandatory format → SKILL.md, workflow → references |
| `self_improvements` | `self-improve` | Project structure → SKILL.md, build guide → references |
| `knowledge_retrieval` | `knowledge-retrieval` | Tool cheat sheet → SKILL.md, scenarios → references |
| `implementation_plan` | `implementation-plan` | Clarified plan/don't-code distinction |
| `fix_comments` | `fix-comments` | Added ddoc handling, 4-category classification |
