# Fix Comments Strategy (LLM Agent)

A structured protocol for an LLM agent to audit and clean up source code comments. **Use this when** asked to review, remove, or improve comments in code files — especially to eliminate redundancy, fix inaccuracies, or enforce a "code-first" documentation philosophy.

## Core Principle

Comments should explain **why** and **what's non-obvious**, not **what** the code literally does. If the code is self-documenting, the comment is redundant.

## 1. Discovery Phase

- **List Target Files**: Identify all files in the specified directory/path.
- **Read Files Completely**: Load each file in full (use chunked reads if needed). Note line counts and structure.
- **Catalog All Comments**: Identify every comment, noting:
  - Line number(s)
  - Comment type (block doc `/** */`, line doc `///`, inline `//`)
  - Content summary
  - Adjacent code (what the comment describes)

## 2. Classification Phase

For each comment, classify into one of four categories:

### KEEP — Non-Obvious Information
The comment explains something **not evident from the code alone**:
- Architectural decisions (why a pattern was chosen)
- Non-obvious edge cases or gotchas
- External system behavior (API quirks, C library constraints)
- Thread-safety notes, lifecycle guarantees
- Specific initialization values or relationships not obvious from names
- Error handling contracts and failure modes
- License information
- Module/class-level documentation explaining purpose and design

### REMOVE — Redundant
The comment restates what the code already shows:
- "Returns X" when the return type and code make it obvious
- "Sets Y to Z" when the assignment is literal
- Variable descriptions matching the variable name (`// Token array` before `tokens = new Token[n]`)
- Function descriptions matching the function name (`// Create params` before `make()`)
- Parameter lists for function calls when parameter names are clear
- Section separators that don't add semantic value (`// --- Unit tests ---`)
- Task/ticket references (`// Task 15: ...`) — not relevant to implementation

### REMOVE — Wrong/Outdated
The comment contradicts the actual code:
- Describes behavior the code doesn't implement
- References removed features or old APIs
- Incorrect technical claims

### UNSURE — Keep and Flag
When uncertain, **keep the comment** and note it in the summary with:
- Location (file:line)
- Comment text
- Why it might be removable
- Why you're uncertain

## 3. Decision Rules (Concrete Criteria)

### Always Remove
| Pattern | Example | Reason |
|---------|---------|--------|
| Task references | `// --- Task 15: Configurable buffer ---` | Process artifact, not implementation |
| Obvious variable descriptions | `// Token array` before `tokens = new T[n]` | Name + type + code = full info |
| Obvious method descriptions | `/// Access the raw pointer.` before `ptr() { return _ptr; }` | Signature says it all |
| Obvious control flow | `// First pass: query size` before size query code | Code structure shows it |
| Duplicate parameter docs | `// func(a, b, c)` before `func(a, b, c)` | Redundant listing |
| Obvious null/error checks | `// Null check prevents crash` before `if (x is null)` | Defensive pattern is evident |
| Section markers | `// --- Unit tests ---` | Visual noise, no semantic value |

### Always Keep
| Pattern | Example | Reason |
|---------|---------|--------|
| Architecture explanation | "Uses facade pattern because..." | Not visible in code |
| Non-obvious edge cases | "Zero-token batch causes undefined behavior in C lib" | Critical gotcha |
| External API behavior | "llama_encode doesn't use KV cache unlike llama_decode" | External knowledge |
| Thread-safety notes | "NOT thread-safe: data race on _destroyed flag" | Critical contract |
| Lifecycle guarantees | "Idempotent — safe to call multiple times" | Behavioral contract |
| Specific init values | "Pre-allocated with all 1s (one per token)" | Value not in name |
| Field relationships | "Each entry points to corresponding _storage entry" | Relationship not obvious |
| Module/class docs | Purpose, design rationale, interface explanation | Context for readers |
| License info | `License: MPL-2.0` | Legal requirement |

### Keep in Unit Tests
- Test purpose descriptions (what scenario is being tested)
- Test limitation notes (why full testing isn't possible)
- Integration test instructions (how to enable with real data)

## 4. Execution Phase

### For Single-File Changes
1. **Read the file** completely.
2. **Apply edits** using `editFile` for targeted removals.
3. **Verify immediately** by re-reading affected sections.

### For Multi-File or Bulk Changes
1. **Read all files** first. Complete the classification before any edits.
2. **Write a script** (Python preferred) to perform bulk edits:
   - Track line numbers carefully (adjust for shifts after each removal).
   - Prefer pattern-based removal over line-number-based when possible.
3. **Execute the script** and verify results.
4. **Clean up artifacts**: Remove extra blank lines, fix indentation issues.

### Edit Safety Rules
- **Never edit based on assumed line numbers** — always verify current state.
- **After each batch of edits**, re-read the affected area to confirm correctness.
- **If edits corrupt the file**, stop and assess damage before continuing.
- **Preserve code structure**: Don't remove blank lines that separate logical sections (keep max 1 blank line between sections).

## 5. Verification Phase

After all edits:
- [ ] Re-read each modified file to confirm no code was accidentally removed.
- [ ] Verify indentation is consistent with file conventions.
- [ ] Check that no extra blank lines were introduced.
- [ ] Confirm all kept comments are still relevant to adjacent code.
- [ ] Ensure doc comment blocks (`/** */`) are properly closed.

## 6. Output Format

Produce a summary in this structure:

```markdown
## Files Modified

### [filename] ([new_line_count] lines, was [old_line_count])
**Removed (N comments):**
- Line X: `[comment text]` — [reason: obvious from code / task ref / wrong]
- Line Y: `[comment text]` — [reason]

**Kept (notable):**
- Line A: `[comment text]` — [reason: explains non-obvious behavior]

### [filename] — No changes needed
[Brief reason]

## Comments Flagged as Unsure
(None, or list with location, text, and reasoning)
```

## 7. Anti-Patterns to Avoid

- **Over-removal**: Don't strip all comments. Good comments add signal.
- **Line-number drift**: After removing lines, all subsequent line numbers shift. Track this carefully.
- **Ignoring context**: A comment might seem redundant in isolation but provide crucial context when the file is skimmed.
- **Breaking doc tools**: Don't remove `///` or `/** */` blocks that feed documentation generators unless explicitly asked.
- **Modifying test comments aggressively**: Test comments often explain intent that assertions alone don't show.

## 8. Error Recovery

If edits go wrong:
1. **Stop immediately**. Don't compound errors.
2. **Assess damage**: Read the file to understand what's broken.
3. **If possible, reverse**: Use git checkout or rewrite from known-good state.
4. **If no backup**: Read remaining content, reconstruct missing parts from context, and rewrite carefully.
5. **Consider script-based approach**: For bulk changes, a Python script with careful line tracking is safer than sequential `editFile` calls.
