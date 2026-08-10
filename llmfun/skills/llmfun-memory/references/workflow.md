# Llmfun Memory Workflow — Detailed Steps

## Step 1: Identify Need

Recognize new learning that should be stored. Store in memory when:
- You made a mistake that cost time to debug
- You discovered non-obvious behavior (API, tool, language)
- A pattern repeats across 2+ different tasks
- User reveals a preference, convention, or project-specific detail
- You found a workaround for a tool limitation
- You solved a problem in a way you'd want to remember

Do NOT store:
- Common knowledge that doesn't require lookup
- Temporary session-specific state
- Information already in the RAG index
- Speculative ideas that haven't been verified

## Step 2: Retrieve

- Call `getMemoryTopics` to list all stored topics with summaries.
- For any topic that could be relevant, call `readMemory` to fetch its content.

## Step 3: Compare

Analyze old vs new information:
- Are there gaps in the existing entry?
- Is there redundant information?
- Do the new findings contradict existing notes?
- Can the new information be merged logically?

## Step 4: Decide — Update vs Create

### Update Existing Memory When:
- New findings are related to an existing topic
- You've discovered better practices for something already documented
- You've made mistakes that contradict or improve existing notes
- The new information complements or expands existing content

### Create New Memory Topic When:
- The topic is fundamentally different from existing ones
- You're learning about a completely new subject
- The existing topics don't have a logical place for the new information
- The topic has distinct enough context that merging would cause confusion

## Step 5: Format

Structure content per the mandatory format:

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
- **Topic name**: Use `category_specifics` format (e.g., `language_d`, `tool_file_editing`, `pattern_error_handling`)
- **Last updated**: Always include the current ISO 8601 date at the top
- **Lessons Learned section**: Required. Contains the positive knowledge — what to do, what works, patterns to follow
- **Common Pitfalls section**: Include only when relevant. Contains negative knowledge — what not to do, gotchas, mistakes to avoid
- **Bold the key term** at the start of each bullet for quick scanning
- **Use bullet points** over paragraphs — keep entries scannable
- **Include code examples** for technical lessons where they add clarity
- **Keep it concise** — each lesson should be readable in a few seconds

### Example Entry
```markdown
# language_d
Last updated: 2026-07-07

## Lessons Learned
- **Use `immutable(T)[]` for static collections**: Allows initialization in `shared static this()` with array literals
  - **Context**: Discovered while refactoring template data loading
  - **Application**: Prefer `immutable` over `shared` for static data to avoid compilation complexity
  - **Example**: `immutable(Config)[] configs;` initialized in `shared static this()`

## Common Pitfalls
- **Don't use `.dup` with `@nogc`**: `.dup` allocates memory and violates `@nogc`
  - **Fix**: Return data directly as immutable, no copying needed
- **Don't mix `shared` and `immutable`**: Causes compilation errors
  - **Fix**: Stick to `immutable` for static data
```

## Step 6: Store

Call `writeMemory` with the formatted content.

## Step 7: Cleanup

### Remove When:
- The topic is no longer relevant or useful
- The content has been fully absorbed into another topic
- The topic was temporary (like a session-specific note)
- The information is outdated and contradicts current knowledge

### Before Removing:
- Verify the content isn't needed elsewhere
- Ensure related information has been moved or merged
- Consider if the topic might be needed in future sessions

Call `removeMemory` for obsolete topics.

## Memory Consolidation & Cleanup Protocol

When cleaning up or consolidating memory topics, follow this process:

### Categorize Each Memory Topic

Every memory topic falls into one of two categories:

1. **Reusable Lessons** — Language quirks, tool behavior, environment facts, patterns that apply across tasks. These should be KEPT (distilled to essentials).
2. **Ephemeral Notes** — Task-by-task implementation details, specific bug fixes, one-time design decisions, review outcomes. These should be REMOVED.

### Signs a Memory is Reusable

- Documents a language feature or quirk (e.g., D AA literal syntax, C++ inline static constexpr)
- Describes tool behavior that affects how you work (e.g., editFile line-shift corruption)
- Captures environment quirks (e.g., container shell joining, grep failures)
- Contains a pattern or anti-pattern that applies to future work
- Would be useful to someone new to the codebase or environment

### Signs a Memory is Ephemeral

- References specific task numbers (e.g., "Tasks 1-6", "Task 9")
- Documents a completed implementation's step-by-step process
- Contains review outcomes or approval notes
- Describes a one-time bug fix with no generalizable lesson
- Is a "consolidated" version of many task notes that are now done

### Consolidation Rules

1. **Distill, don't copy:** When recreating a memory from an ephemeral one, extract only the reusable lessons. Strip task references, review notes, and implementation specifics.
2. **Verify before storing:** If a lesson depends on environment state (e.g., a tool bug, a broken build config), verify it's still true before storing. Stale lessons are worse than no lessons.
3. **Merge duplicates:** If multiple memories cover the same topic, merge into one and remove the rest.
4. **Rename for clarity:** Use `category_specifics` naming (e.g., `d_language_lessons`, `tool_usage_lessons`) instead of implementation-specific names (e.g., `io_edit_refactor_consolidated`).
5. **Keep it lean:** A memory should be readable in a few seconds. If it's longer than a page, it's probably too detailed.

### Verification Checklist

Before storing a consolidated memory, verify:
- [ ] Does this lesson still apply to the current codebase/environment?
- [ ] Would a future session actually use this information?
- [ ] Is the lesson stated generally (not tied to a specific task)?
- [ ] Is the entry concise and scannable?
