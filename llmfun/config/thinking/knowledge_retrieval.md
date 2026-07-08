# Knowledge Retrieval Protocol

## Core Principle
This protocol applies to your **current active objective**—the specific fact, code, or concept you need right now to complete your immediate sub-task. This is **not** necessarily the user's original utterance.

## Hard Cap (STRICT)
You have a maximum of **10 tool calls** per distinct knowledge-seeking objective. Includes `listRAGDatabases`, `queryTextSearch`, `querySemantic`, `queryBestMatch`, and `queryReadFile`.

## Phase 0: Discovery & First Pass (Call 1 - MANDATORY)
Execute these **simultaneously** in your very first turn:

1. **Call `queryBestMatch`** with your current objective phrased as a natural question or set of keywords.
2. **Call `listRAGDatabases`** to discover available database names.

**Why this default?** `queryBestMatch` uses RRF to fuse semantic and FTS5 results. Even if your FTS5 query is poorly crafted, the semantic half will still pull in relevant content. This single call gives you a robust, balanced result set 80% of the time.

## Phase 1: The Intentional Pivot (Calls 2-4)
Read your `queryBestMatch` results. **Do not blindly switch tools.** Pivot *only* based on a specific diagnosis:

### Scenario A: The results are too noisy or diluted (RRF is pulling in too much)
- **Symptom:** You got 100+ results, but the top 10 are only tangentially related to your objective.
- **Action:** Re-run your search **scoped to the most specific database** discovered in Phase 0. Keep using `queryBestMatch` with the `database` parameter. The narrower corpus will improve the RRF ranking.

### Scenario B: You know a unique, exact keyword (function name, error code, file path)
- **Symptom:** You are absolutely certain a specific, non-generic string exists in the docs (e.g., `authenticate_user_v2`, `ERR_CONFIG_MISSING`).
- **Action:** Call `queryTextSearch` scoped to your discovered database. **Warning:** Only do this if the keyword is highly distinctive. Generic terms like `"user"`, `"data"`, or `"login"` will cause FTS5 to return zero (implicit AND) or thousands of garbage hits.

### Scenario C: Your objective is purely conceptual, with no concrete terms
- **Symptom:** You are asking *"How does the event-driven architecture work?"* or *"What is the overall flow?"* and `queryBestMatch` returned overly specific code snippets.
- **Action:** Call `querySemantic` scoped to your database with a broad, natural-language version of your question. This ignores keywords entirely and searches for meaning.

### Scenario D: `queryBestMatch` returned zero results (rare with RRF, but possible)
- **Action:** Immediately call `querySemantic` (broad) and `queryTextSearch` (with your most unique noun) **in parallel** as a Hail Mary.

## Phase 2: Digging and Reading (Calls 5-8)
Once you find a relevant document snippet that mentions specific line numbers:

- **`queryReadFile` takes a single `lineNumber`.** Read the **single most critical line** first. The surrounding context is usually visible in the search snippet.
- You can parallelize up to two `queryReadFile` calls if you have two equally critical lines to verify.

## Phase 3: Verification (Calls 9-10)
- Use **Call 9** to verify a critical quoted phrase. Stick with `queryBestMatch` scoped to your database—RRF will catch it if it exists.
- Use **Call 10** for one final `queryReadFile` to confirm your exact quote before citing it.
- **After Call 10, STOP.** Synthesize your answer.

## The "Partial Answer" Rule
If after 6-7 calls you still lack a complete picture:
- Do **not** use remaining calls to chase new leads. Use them only to verify what you have.
- Respond transparently: *"I found [X] (e.g., line 42 of auth.py), but [Y] was not found. Proceeding with [X]."*

## Tool Selection Cheat Sheet (The 80/20 Rule)

| Tool | When to Use | When NOT to Use |
| :--- | :--- | :--- |
| **`queryBestMatch`** | **Default for 80% of searches.** Use it first, use it often, use it scoped to a DB for noise reduction. | Only if you have a very specific reason to use the other two. |
| **`querySemantic`** | When your objective is purely conceptual, broad, or lacks specific nouns. | When you know a unique function name or error code. |
| **`queryTextSearch`** | **Only** when you possess a highly unique, exact, non-generic keyword. | If the keyword is generic (e.g., "user", "data", "login")—it will fail or flood. |

## Tool Definitions (For reference)
You have four search/discovery tools for the external knowledge base. Choose based on your query type:

- **`queryBestMatch`** (Combined): Merges semantic and FTS scoring. Use when you want broad coverage, but be aware that for very keyword-specific queries the semantic component may dilute precision by ranking conceptually related but topically irrelevant documents higher.

- **`querySemantic`** (Vector Search): Best for conceptual queries, natural language questions, or when you're searching for ideas rather than exact terms. Useful when synonyms or paraphrasing may be used in the indexed content.

- **`queryTextSearch`** (Full-Text Search): Best for keyword-heavy queries with specific terms, proper nouns, file names, function names, or when you know the exact words to search for. FTS matches exact text occurrences precisely.

- **`queryReadFile`** (Exact Line Lookup): Retrieves the exact text chunk(s) containing a specific line number from a file in the RAG index. Use when you need to read precise content from a known file at a known line. Supports `database` parameter for scoping and `appendLoc` for line number prefixes.

- **`listRAGDatabases`** (Discovery): Lists all available RAG databases with their names and file paths. Use this to discover database names for filtering queries with the `database` parameter.

**Database Parameter**: All query tools that accept a database parameter (`querySemantic`, `queryTextSearch`, `queryBestMatch`, `queryReadFile`) restrict the search to the database with that name. Pass the string (`"*"`) to search all databases (default behavior). Use `listRAGDatabases` to discover available database names before filtering.
