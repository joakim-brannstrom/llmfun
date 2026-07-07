A strategy for updating memory. **Use this when** updating or creating memory entries about topics you encounter during work.

## Standard Memory Entry Format (MANDATORY)

Every memory entry MUST follow this structure. Deviations are not allowed.

```markdown
# [Topic Name]
Last updated: [ISO 8601 date]

## Lessons Learned
- **Key Point**: Concise statement of the lesson
  - **Context**: Brief description of when/why this was discovered
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

## When to Update vs Create New Memory

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

## Memory Update Workflow

1. **Identify Need**: Recognize new learning that should be stored
2. **Retrieve**: Call `getMemoryTopics`, then `readMemory` for relevant topic
3. **Compare**: Analyze old vs new information — identify gaps, redundancies, contradictions
4. **Decide**: Update existing topic OR create new topic (use criteria above)
5. **Format**: Structure content per the mandatory format above
6. **Store**: Call `writeMemory` with the formatted content
7. **Cleanup**: Call `removeMemory` for obsolete topics if needed

## When to Remove Old Memory Topics

### Remove When:
- The topic is no longer relevant or useful
- The content has been fully absorbed into another topic
- The topic was temporary (like a session-specific note)
- The information is outdated and contradicts current knowledge

### Before Removing:
- Verify the content isn't needed elsewhere
- Ensure related information has been moved or merged
- Consider if the topic might be needed in future sessions

## Common Mistakes to Avoid

- **Overwriting without reviewing**: Always read existing memory with `readMemory` first
- **Creating duplicates**: Check `getMemoryTopics` before creating new topics
- **Storing raw data**: Memory should contain lessons, not just facts
- **Inconsistent formatting**: Follow the mandatory format — no deviations
- **Too verbose**: Keep entries concise and scannable
- **Missing context**: Include when/why lessons apply, not just what
- **Forgetting to update date**: Always set `Last updated` to current date
