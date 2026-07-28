---
name: llmfun-memory
description: >-
  Manage llmfun memory entries: store lessons learned, common pitfalls, and
  discovered patterns. Use when you need to remember something useful for future
  sessions, update existing knowledge, or clean up obsolete memory topics.
  Triggers on: update memory, store memory, remember, lesson learned, common
  pitfall, memory entry, llmfun-memory, write memory, remove memory, memory
  management.
version: 1.0.0
---

# Llmfun Memory Skill

Manage llmfun memory entries. Store lessons learned, common pitfalls, and
discovered patterns for retrieval in future sessions.

## When to Use

Use this skill when:
- You discover something worth remembering for future sessions
- You make a mistake that should be documented to avoid repetition
- A pattern repeats across 2+ different tasks
- You find a workaround for a tool limitation
- You need to update existing memory with new findings
- Obsolete memory topics need cleanup

## Entry Format (MANDATORY)

Every memory entry MUST follow this structure:

```markdown
# [Topic Name]
Last updated: [ISO 8601 date]

## Lessons Learned
- **Key Point**: Concise statement of the lesson
  - **Context**: When/why this was discovered
  - **Application**: How to apply this going forward
  - **Example**: Code snippet or concrete scenario (when applicable)

## Common Pitfalls
- **Pitfall**: What goes wrong or what to avoid
  - **Fix**: How to avoid or resolve it
```

### Format Rules
- **Topic name**: Use `category_specifics` format (e.g., `language_d`, `tool_file_editing`)
- **Last updated**: Always include current ISO 8601 date
- **Lessons Learned**: Required — what to do, what works, patterns to follow
- **Common Pitfalls**: Include only when relevant — what not to do, gotchas
- **Bold key terms** at start of each bullet for quick scanning
- **Use bullet points** over paragraphs — keep entries scannable
- **Include code examples** for technical lessons where they add clarity
- **Keep it concise** — each lesson should be readable in a few seconds

## Workflow

Follow the protocol. See `references/workflow.md` for detailed steps.

1. **Identify Need** — Recognize new learning that should be stored.
2. **Retrieve** — Call `getMemoryTopics`, then `readMemory` for relevant topic.
3. **Compare** — Analyze old vs new information — identify gaps, redundancies, contradictions.
4. **Decide** — Update existing topic OR create new topic (use criteria in references).
5. **Format** — Structure content per the mandatory format above.
6. **Store** — Call `writeMemory` with the formatted content.
7. **Cleanup** — Call `removeMemory` for obsolete topics if needed.

## Common Mistakes to Avoid

- **Overwriting without reviewing**: Always read existing memory with `readMemory` first
- **Creating duplicates**: Check `getMemoryTopics` before creating new topics
- **Storing raw data**: Memory should contain lessons, not just facts
- **Inconsistent formatting**: Follow the mandatory format — no deviations
- **Too verbose**: Keep entries concise and scannable
- **Missing context**: Include when/why lessons apply, not just what
- **Forgetting to update date**: Always set `Last updated` to current date

## References

- Detailed workflow and decision criteria: `references/workflow.md`
