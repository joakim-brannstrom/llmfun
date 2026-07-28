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
