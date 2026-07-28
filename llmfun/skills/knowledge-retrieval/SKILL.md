---
name: knowledge-retrieval
description: >-
  Structured protocol for searching and retrieving knowledge from RAG databases.
  Use when you need to find facts, code, or concepts to complete your current
  task. Triggers on: knowledge retrieval, search, find information, look up,
  query database, RAG search, documentation lookup, verify fact, check docs.
version: 1.0.0
---

# Knowledge Retrieval Skill

Structured protocol for searching and retrieving knowledge from RAG databases.
Applies to your **current active objective** — the specific fact, code, or
concept you need right now to complete your immediate sub-task.

## Core Principle

Search for what you need **now** to complete your current sub-task. This is not
necessarily the user's original utterance — it's the specific information gap
blocking your progress.

## Hard Cap (STRICT)

Maximum **10 tool calls** per distinct knowledge-seeking objective. Includes
`listRAGDatabases`, `queryTextSearch`, `querySemantic`, `queryBestMatch`, and
`queryReadFile`.

## Workflow

Follow the phased protocol. See `references/protocol.md` for detailed steps.

1. **Phase 0: Discovery** (Call 1) — Call `queryBestMatch` and `listRAGDatabases` simultaneously.
2. **Phase 1: Pivot** (Calls 2-4) — Diagnose results and pivot strategy based on specific symptoms.
3. **Phase 2: Dig** (Calls 5-8) — Read specific lines from relevant documents.
4. **Phase 3: Verify** (Calls 9-10) — Verify critical quotes, then stop and synthesize.

## Tool Selection

| Tool | When to Use | When NOT to Use |
|------|-------------|-----------------|
| `queryBestMatch` | **Default for 80% of searches.** First call, scoped for noise reduction | Only if you have a specific reason for other tools |
| `querySemantic` | Purely conceptual, broad queries, no concrete terms | When you know a unique function name or error code |
| `queryTextSearch` | **Only** with highly unique, exact, non-generic keywords | With generic terms ("user", "data", "login") — will fail or flood |
| `queryReadFile` | Read specific line from known file in RAG | When you don't know the file or line number |
| `listRAGDatabases` | Phase 0 — discover available database names | After you already know the database name |

## Key Rules

- **Default to `queryBestMatch`**: It fuses semantic + FTS5 results. Use it first, use it often.
- **Scope to databases**: After Phase 0, restrict searches to specific databases for precision.
- **Partial answers are OK**: After 6-7 calls, verify what you have. Don't chase new leads.
- **Stop at 10 calls**: Synthesize your answer. No more searches.

## References

- Full protocol with scenarios: `references/protocol.md`
