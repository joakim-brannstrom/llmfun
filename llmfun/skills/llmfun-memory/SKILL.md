---
name: llmfun-memory
description: >-
  Manage llmfun memory entries: store lessons learned, common pitfalls, and
  discovered patterns. Use when you need to remember something useful for future
  sessions, update existing knowledge, or clean up obsolete memory topics.
  Triggers on: update memory, store memory, remember, lesson learned, common
  pitfall, memory entry, llmfun-memory, write memory, remove memory, memory
  management.
version: 1.2.0
---

# Llmfun Memory Skill

## Entry Format (MANDATORY)

Every memory entry MUST follow this structure:

```markdown
# [Topic Name]

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
- **No dates in content**: retrieval surfaces filesystem `Last modified` automatically — never write dates into memory content
- **Lessons Learned**: Required — what to do, what works
- **Common Pitfalls**: Only when relevant — what not to do, gotchas
- **Bold key terms** at start of each bullet for quick scanning
- **Use bullet points** over paragraphs — keep entries scannable
- **Include code examples** for technical lessons where they add clarity
- **Keep it concise** — each lesson readable in a few seconds

## Workflow

See `references/workflow.md` for detailed steps.

1. **Identify Need** — Recognize new learning that should be stored.
2. **Retrieve** — `getMemoryTopics` lists short-term (`short/`, default 7 days)
   topics only; long-term (`long/`, default 100 days) topics are RAG-indexed —
   discover via RAG search (`queryBestMatch`/`querySemantic`).
   `readMemory` reads either tier.
3. **Compare** — Analyze old vs new — identify gaps, redundancies,
   contradictions.
4. **Decide** — Update existing topic OR create new topic (use criteria in
   references).
5. **Format** — Structure content per the mandatory format above.
6. **Store** — Call `writeMemory`. On an existing long-term topic the first write
   FAILS unless its content already includes the stored content (then it IS the
   replace/merge write and lands in `short/`); it returns the stored content and
   nothing is written. Otherwise the second write (stored content included)
   decides replace vs merge and lands in `short/`. Promotion/demotion happen
   automatically in the background.
7. **Cleanup** — Call `removeMemory` for obsolete topics; it works on both tiers;
   archived (`archive/`) ones are unreachable (reported as nonexistent).

## Common Mistakes

- **Overwriting without reviewing**: Always read existing memory with `readMemory`
  first; on an existing long-term topic the first `writeMemory` typically FAILS
  (it succeeds only if its content already embeds the stored content) and returns
  the stored content — write it again fully (replace) or stored + additions
  (merge). The failed write is not saved.
- **Creating duplicates**: Check `getMemoryTopics` and RAG search first
- **Storing raw data**: Memory should hold lessons, not just facts
- **Inconsistent formatting**: Follow the mandatory format — no deviations
- **Too verbose**: Keep entries concise and scannable
- **Missing context**: Include when/why lessons apply, not just what
- **Keeping ephemeral notes**: Task-specific details, review outcomes, and one-time bug fixes should NOT be stored — only reusable lessons belong in memory.
- **Stale environment lessons**: verify a lesson about tool behavior or environment quirks is still true before storing — a lesson about a fixed bug is worse than none.

## References

- Detailed workflow and decision criteria: `references/workflow.md`
- The 'Memory Cleanup Protocol' section in `references/workflow.md`
