A strategy for updating memory. **Use this when** updating or creating memory entries about topics you encounter during work.

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

## How to Merge Old and New Information

### Step 1: Retrieve and Review
- Always call `readMemory` for the topic before updating
- Understand what's already documented
- Identify gaps, redundancies, and contradictions

### Step 2: Analyze Differences
- Compare new findings with existing content
- Identify what's redundant (same information, different wording)
- Identify what's complementary (new insights on same topic)
- Identify what's contradictory (new information that conflicts)

### Step 3: Restructure for Clarity
- Group related concepts under clear headings
- Use consistent formatting (bullet points, bold keywords, code blocks)
- Remove duplicate entries that convey the same lesson
- Reorder items logically (by importance, by category, by complexity)

### Step 4: Write Comprehensive Update
- Keep the best of both old and new information
- Resolve contradictions by preferring verified/working solutions
- Use clear, actionable language
- Include code examples where helpful
- Add context about when/why each lesson applies

## Avoiding Redundancy

### Check for Duplicates Before Writing:
- Look for the same concept described differently
- Check if multiple bullets convey identical lessons
- Verify that sections don't overlap significantly

### Consolidation Techniques:
- Merge related bullet points into single, comprehensive entries
- Combine similar patterns into general rules with specific examples
- Use hierarchical structure (main points with sub-points)

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

## Best Practices for Memory Content

### Format Consistently:
```markdown
# Topic Name

## Category 1
- **Key Point**: Explanation with context
- **Example**: Code or scenario

## Category 2
- Similar structure
```

### Keep Entries Concise:
- Use bullet points over paragraphs when possible
- Bold key terms for quick scanning
- Include code examples for technical lessons
- Avoid unnecessary explanation of well-known concepts

### Make Actionable:
- Focus on lessons learned, not just facts
- Include "how" and "why" not just "what"
- Note common pitfalls and how to avoid them
- Reference existing utilities or patterns to reuse

### Verify Before Storing:
- Confirm the information is correct
- Prefer verified solutions over speculative ones
- Note when something is experimental or untested
- Update if new information proves old notes wrong

## Memory Update Workflow

1. **Identify Need**: Recognize new learning that should be stored
2. **Retrieve**: Call `readMemory` to get existing content
3. **Compare**: Analyze old vs new information
4. **Plan Structure**: Decide on headings and organization
5. **Write**: Create comprehensive, non-redundant content
6. **Verify**: Double-check for accuracy and completeness
7. **Store**: Call `writeMemory` with updated content
8. **Cleanup**: Remove obsolete topics if needed

## Common Mistakes to Avoid

- **Overwriting without reviewing**: Always read existing memory first
- **Creating duplicates**: Check for similar topics before creating new ones
- **Storing raw data**: Memory should contain lessons, not just facts
- **Forgetting to remove**: Delete topics that are no longer useful
- **Inconsistent formatting**: Use consistent structure across topics
- **Too verbose**: Keep entries concise and scannable
- **Missing context**: Include when/why lessons apply, not just what

