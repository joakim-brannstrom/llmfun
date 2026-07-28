# Knowledge Retrieval Protocol — Detailed Steps

## Phase 0: Discovery & First Pass (Call 1 — MANDATORY)

Execute these **simultaneously** in your very first turn:

1. **Call `queryBestMatch`** with your current objective phrased as a natural question or set of keywords. For this first query, the `textQuery` parameter should contain only one keyword — the most relevant.
2. **Call `listRAGDatabases`** to discover available database names.

**Why this default?** `queryBestMatch` uses RRF to fuse semantic and FTS5 results. Even if your FTS5 query is poorly crafted, the semantic half will still pull in relevant content. This single call gives you a robust, balanced result set 80% of the time.

## Phase 1: The Intentional Pivot (Calls 2-4)

Read your `queryBestMatch` results. **Do not blindly switch tools.** Pivot *only* based on a specific diagnosis:

### Scenario A: Results are too noisy or diluted
- **Symptom**: You got many results, but the top hits are only tangentially related.
- **Action**: Re-run search **scoped to the most specific database** from Phase 0. Keep using `queryBestMatch` with the `database` parameter. The narrower corpus improves RRF ranking.

### Scenario B: You know a unique, exact keyword
- **Symptom**: You are certain a specific, non-generic string exists (e.g., `authenticate_user_v2`, `ERR_CONFIG_MISSING`).
- **Action**: Call `queryTextSearch` scoped to your discovered database. **Warning**: Only if the keyword is highly distinctive. Generic terms will return zero or thousands of hits.

### Scenario C: Objective is purely conceptual
- **Symptom**: You're asking "How does X work?" and got overly specific code snippets.
- **Action**: Call `querySemantic` scoped to your database with a broad, natural-language version of your question. This ignores keywords and searches for meaning.

### Scenario D: Zero results (rare)
- **Action**: Immediately call `querySemantic` (broad) and `queryTextSearch` (with your most unique noun) **in parallel** as a Hail Mary.

## Phase 2: Digging and Reading (Calls 5-8)

Once you find a relevant document snippet that mentions specific line numbers:

- **`queryReadFile` takes a single `lineNumber`**. Read the **single most critical line** first. Surrounding context is usually visible in the search snippet.
- You can parallelize up to two `queryReadFile` calls if you have two equally critical lines to verify.

## Phase 3: Verification (Calls 9-10)

- Use **Call 9** to verify a critical quoted phrase. Stick with `queryBestMatch` scoped to your database — RRF will catch it if it exists.
- Use **Call 10** for one final `queryReadFile` to confirm your exact quote before citing it.
- **After Call 10, STOP.** Synthesize your answer.

## The "Partial Answer" Rule

If after 6-7 calls you still lack a complete picture:

- Do **not** use remaining calls to chase new leads. Use them only to verify what you have.
- Respond transparently: *"I found [X] (e.g., line 42 of auth.py), but [Y] was not found. Proceeding with [X]."*

## Tool Definitions

- **`queryBestMatch`** (Combined): Merges semantic and FTS scoring. Best for broad coverage. Note: for very keyword-specific queries, the semantic component may dilute precision.
- **`querySemantic`** (Vector Search): Best for conceptual queries, natural language questions, or searching for ideas rather than exact terms. Useful when synonyms or paraphrasing may be used.
- **`queryTextSearch`** (Full-Text Search): Best for keyword-heavy queries with specific terms, proper nouns, file names, function names, or when you know the exact words. FTS matches exact text occurrences precisely.
- **`queryReadFile`** (Exact Line Lookup): Retrieves the exact text chunk(s) containing a specific line number from a file in the RAG index. Use when you need to read precise content from a known file at a known line.
- **`listRAGDatabases`** (Discovery): Lists all available RAG databases with their names and file paths. Use to discover database names for filtering queries.

## Database Parameter

All query tools that accept a database parameter restrict the search to the database with that name. Pass `"*"` to search all databases (default). Use `listRAGDatabases` to discover available database names before filtering.
