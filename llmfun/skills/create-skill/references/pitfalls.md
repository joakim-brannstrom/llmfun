# Common Pitfalls to Avoid

| Pitfall | Consequence | Fix |
|---------|-------------|-----|
| Directory name ≠ `name` field | Warning logged; may confuse agent | Always match them exactly |
| `name` has uppercase or underscores | Skill may fail to load | Use only lowercase, numbers, hyphens |
| `description` is too vague | Agent won't know when to trigger | Include both "what" and "when" plus trigger keywords |
| SKILL.md > 100 lines | Too many tokens on activation | Split into `references/` |
| SKILL.md > 4096 bytes | **Truncated on load** — agent loses instructions | Keep body under 4KB; move details to `references/` |
| All logic inline | Bloated, hard to maintain | Use `scripts/` and `references/` |
| No trigger keywords in description | Agent may not activate when needed | Add `Triggers on:` keyword list |
| Duplicate content in SKILL.md and references | Wasted tokens, maintenance burden | Each piece of info lives in exactly one place |
| Overly long descriptions | Wasted tokens, unclear triggers | Keep under 1024 characters, focus on key triggers |
| No version in frontmatter | Hard to track changes | Always include `version: 1.0.0` at minimum |
| Forgetting to update the date in memory entries | Stale information looks current | Always set `Last updated` to current ISO 8601 date |
| References to files that don't exist | Agent gets "file not found" error | Verify all referenced files exist before reporting done |
