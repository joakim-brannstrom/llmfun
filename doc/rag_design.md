# Design Document: Agentic RAG Retrieval Loop

---

## 1. Overview & Purpose

This document describes the architecture and operational logic of our RAG (Retrieval-Augmented Generation) pipeline. Unlike traditional RAG systems that rely on a static retriever + cross-encoder reranker, our system utilizes an **Agentic Retrieval Loop**.

The LLM acts as an autonomous researcher, equipped with multiple search tools, to actively hunt for, read, and verify information from a corpus of knowledge bases (databases). The system is designed to handle complex, multi-step coding and technical queries where the relevant information might span multiple files or require follow-up "chunk chasing."

Two companion documents cover the rest of the system: `rag_design2.md` documents the indexing, embedding, and fusion pipeline internals (chunkers, embedding layout contract, the three query paths, name resolution, evaluation harness), and `database.md` documents the SQL/FTS5/RRF behavior. Section 9 below records the design's history — the pitfalls we hit, the approaches that didn't work, and what prevents regressions now.

---

## 2. Core Philosophy: Why No Traditional Reranker?

### The Traditional Approach (Bi-Encoder + Cross-Encoder)
1. **Retrieve:** Fetch Top-50 vector candidates (fast, shallow).
2. **Rerank:** Feed those 50 into a Cross-Encoder model to score them against the query (slow, deep).
3. **Generate:** Feed Top-5 to the LLM.

### Our Approach (The Agentic Loop)
We deliberately **forego** a static reranker. Here is why:

| Feature | Reranker | Agentic Loop (Ours) |
| :--- | :--- | :--- |
| **Interaction** | 1 round. Static. | Multiple rounds. Dynamic. |
| **Chunk Chasing** | Cannot chase cut-off text. If the answer straddles 2 chunks, it fails. | Uses `queryReadFile` to jump to exact lines to retrieve adjacent content. |
| **Strategy Switching** | Only looks at the *original* query. | If semantic search yields high-level fluff, the agent pivots to exact keyword search (FTS5) to find function names. |
| **"Lost Treasure"** | If the answer isn't in the Top-50, it is missed forever. | The agent refines search terms and retries until it finds the data or exhausts its budget. |

**Trade-off:** We trade *predictable latency* (fixed ~500ms for a reranker) for *higher accuracy*. To prevent infinite loops, the LLM is instructed via the skill to limit itself to 10 tool calls per objective, with a "Partial Answer" safety valve at 6-7 calls. Code-enforced limits (consecutive-failure detection) provide a last-resort safety net.

*(Note: While we don't have a Cross-Encoder, the LLM's internal reasoning effectively acts as a dynamic, multi-pass reranker, discarding irrelevant chunks as it reads them).*

---

## 3. Architecture: Tool Suite

The system exposes seven core tools to the LLM. All search tools support a `database` parameter (scoping) or `"*"` (global).

| Tool | Function | Use Case | Key Constraint |
| :--- | :--- | :--- | :--- |
| **`listRAGDatabases`** | Discovers available DB names. | **Discovery.** Called once per objective to map the terrain. | Must be called before scoping searches. |
| **`queryBestMatch`** | RRF (Reciprocal Rank Fusion) merging FTS5 + Vector. | **The 80% Default.** Balances keyword matching with conceptual meaning. Handles poorly crafted queries gracefully. | Returns a mixed bag; can be noisy on wide scopes (`"*"`). |
| **`querySemantic`** | Vector/Embedding search. | **Conceptual broad strokes.** Use when objective lacks specific nouns, or when FTS5 fails. | Fast, but misses exact function names. |
| **`queryTextSearch`** | FTS5 full-text search. Words are implicit `AND`; also `AND`/`OR`/`NOT` (NOT is binary: `x NOT y`), `( )` groups, prefix `term*`, `^term`, `NEAR(t1 t2, N)` (see `doc/database.md`). | **Precision.** Use only when you possess a highly unique, non-generic keyword (e.g., `authenticate_user_v2`). | **Warning:** Generic terms (e.g., "user", "login") cause implicit `AND` to return zero or flood results. |
| **`listRAGSources`** | Lists the indexed documents (paths/topics/URLs) with chunk counts; optional substring `filter` + `limit`. | **Name → path resolution.** When the user names a document, or a result references one by name, resolve its exact indexed path before reading. | Listing is not searching: it finds documents by name, not content. |
| **`readRAGSource`** | Reads a whole document (reconstructed from its chunks). Resolves bare file names (any path suffix). | **Reading referenced documents.** The cheapest way to read a document that another result referenced. | Bounded by `maxBytes` (default 64 KiB, truncation is reported). |
| **`queryReadFile`** | Exact line lookup in the index; also resolves bare file names. | **Chunk Chasing.** Grabs the exact raw text of specific lines. | Best when a line number is known; for full documents use `readRAGSource`. |

### 3.1 Source-Aware Chunk Embedding

Every chunk is embedded with its source prepended as a prefix — `Topic: <name> | `, `Url: <url> | ` or `File: <path> | ` — before it is sent to the embedder. Only the *embedding* carries the prefix; the stored chunk text (and therefore the FTS5 index) does not.

- **Why:** the embedder model can "see" which source a chunk came from, so semantic queries about the source itself work — e.g. *"what does file X say about retries"* matches that file's chunks even though no chunk text mentions the file name.
- **Chunk budget:** the prefix eats into the embedder's batch budget, so the chunk window is `batchSize - prefixLength`. In the token-based chunker the budget is exact by construction (the window is built in tokens and the model's special tokens are reserved up front), and a chunk that is still rejected is dropped with a `warningf` rather than vanishing silently. The grapheme-based chunker additionally has a per-chunk halving fallback for models that reject the input.

---

## 4. Design Rationale: Why the Retrieval Strategy Is Structured This Way

The retrieval strategy is engineered to mitigate the specific cognitive weaknesses of LLMs—particularly poor keyword extraction, suboptimal query planning, and a tendency toward "random walk" behavior—while exploiting their strengths in contextual synthesis and iterative reasoning.

### A. Parallel Discovery Over Sequential Guessing
The skill instructs the LLM to execute `queryBestMatch` and `listRAGDatabases` simultaneously on the first turn.

- **Rationale:** In a sequential system, the LLM would guess a database scope, receive results (or none), guess another scope, and waste calls. By resolving the "search space" (`listRAGDatabases`) and the "content" (`queryBestMatch`) in parallel, the LLM eliminates two variables in a single turn. This immediately reduces the `"*"` wildcard noise problem (by providing concrete database names) without sacrificing broad recall.

### B. RRF as the Robust Default (The 80% Bias)
The protocol heavily biases the LLM toward `queryBestMatch` as the starting point for almost every search.

- **Rationale:** RRF is a fusion algorithm. It ranks results based on their positions in both the FTS5 and semantic result sets. This design choice explicitly compensates for the LLM's tendency to hallucinate or misphrase keywords. Even if the FTS5 query is syntactically wrong or too narrow, the semantic half of the retrieval will still surface conceptually relevant documents. This turns `queryBestMatch` into a highly resilient "safety net" that rarely returns zero results.

### C. Deliberate Restriction of FTS5 (`queryTextSearch`)
The protocol explicitly warns the LLM **not** to use `queryTextSearch` for generic terms and to abandon it immediately if it returns zero.

- **Rationale:** FTS5 uses implicit `AND` logic. When an LLM types `"foo bar batman"`, it is interpreted as `foo AND bar AND batman`. This is almost never how documentation is written. Left unchecked, the LLM will repeatedly attempt FTS5 with minor variations, burning through multiple tool calls with zero yield. By restricting FTS5 to *highly unique, non-generic keywords*, we dramatically reduce the 50% empty-result rate observed in earlier versions. The protocol intentionally treats FTS5 as a *specialized scalpel* rather than a general-purpose hammer.

### D. Diagnostic Pivoting Over Random Switching
Instead of allowing the LLM to cycle through `queryTextSearch` → `querySemantic` → `queryBestMatch` in a blind loop, the protocol forces a **diagnosis** of the failure before pivoting.

- **Rationale:** Empirical observation showed that LLMs often switch tools without understanding *why* the previous tool failed. This leads to "random walk" behavior—wasting calls on irrelevant searches. By categorizing failures as (A) Noise, (B) FTS5 Trap, or (C) Pure Concept, the LLM is forced to match the specific symptom to the correct countermeasure. This turns a chaotic guessing game into a structured, binary-search-like narrowing of the information space.

### E. Deterministic Results (Code-Enforced)

The same query always returns the same results. After the per-database results are collected, the tie-breaking shuffle (`randomizeRanks`) is seeded with a content hash of the query itself, and the rank sort is stable — so repeated identical queries reproduce the identical top-K, while different queries still get different permutations (which is what actually breaks database-order bias among equal-ranked results).

- **Rationale:** the agent is told the database is a stable function of the query. If identical queries returned different top-K sets at the boundary, the agent would conclude the index is unstable and waste calls re-probing. Determinism also makes retrieval bugs reproducible.

### F. Per-Database Interleaving (Code-Enforced)

When several databases are searched (`"*"` scope), the merged top-K cannot be swept by a single database. `takePerSource` truncates the rank-sorted, shuffled result with a round-robin that takes at most 2 matches per database per pass, so the final list interleaves each database's best, next best, ... (it is deliberately NOT strict global rank order).

- **Rationale:** one large database would otherwise dominate every top-K and starve smaller, more specialised ones (e.g. a dedicated design-doc or config database). The 2-per-pass cap guarantees cross-database coverage at the top-K boundary. A database that is the only one with matches can still fill the whole top-K.

---

## 5. Design Rationale: Safety Mechanisms and Cost Control

The safety mechanisms operate at two levels: **prompt engineering constraints** (instructed to the LLM via the skill) and **code-enforced limits** (hard stops in the agent loop). This dual-layer approach provides both graceful guidance and catastrophic failure protection.

### A. The 10-Call Budget (Prompt Engineering Constraint)
The skill (`knowledge-retrieval`) instructs the LLM to use a maximum of **10 tool calls** per distinct knowledge-seeking objective. This includes `listRAGDatabases`, `listRAGSources`, `queryTextSearch`, `querySemantic`, `queryBestMatch`, `queryReadFile`, and `readRAGSource`.

- **Implementation:** This is a soft limit enforced by prompt instructions, not by code. The LLM is told to "STOP" after call 10 and synthesize its answer. The skill divides the budget into stages: stage 0 (Call 1: discovery), stage 1 (Calls 2-4: pivot), stage 2 (Calls 5-8: dig/read), stage 3 (Calls 9-10: verify).
- **Rationale (The Calculus):** Internal telemetry indicates that approximately 50% of all FTS5-based searches return zero results due to the implicit `AND` issue. A budget of 5 would leave the LLM with only 2 to 3 successful reads—insufficient for complex coding queries. A budget of 20 would push latency beyond acceptable thresholds (often exceeding 45-60 seconds) and bloat the context window with failed searches, confusing the model.
- **The Sweet Spot (10):** With 10 calls, the LLM can afford 2 discovery probes, 3 to 4 targeted searches (absorbing the 50% failure rate), 2 to 3 individual line reads (`queryReadFile`), and 2 verification calls. This keeps total execution time under approximately 25-30 seconds while providing enough runway to dig through fragmented documentation.

### B. The Confidence Check (Prompt Engineering Constraint)
The skill instructs the LLM that if, after **6-7 calls**, it does not have a complete answer, it should stop generating new search strategies and reserve the remaining calls exclusively for verification.

- **Implementation:** This is a soft limit enforced by prompt instructions. The LLM is told to respond transparently: *"I found [X] (e.g., line 42 of auth.py), but [Y] was not found. Proceeding with [X]."*
- **Rationale:** The law of diminishing returns applies sharply to RAG retrieval. If the core answer hasn't been found in the first 6 attempts, it is unlikely to be found in the 7th or 8th. Continuing to search at that point merely delays the inevitable. By forcing a shift to verification, we change the failure mode. Instead of timing out with an empty context, the LLM uses the final calls to validate the partial evidence it *does* have. This guarantees that even a "failed" retrieval results in a defensible, partially informed answer rather than a hallucination.

### C. The Reading Budget (queryReadFile vs readRAGSource)
`queryReadFile` takes only a single `lineNumber`, while `readRAGSource` reads a whole document in one call.

- **Rationale:** Reading three specific lines of a file costs three separate `queryReadFile` calls, which is why the budget is 10 rather than a lower number. The 10-call budget intentionally allocates 3-4 calls specifically for line reads, allowing the LLM to chase adjacent code blocks and verify multiple sources without cannibalizing its search budget. For *whole documents* (especially documents referenced by other documents), `readRAGSource` collapses the read into one call — bounded by `maxBytes` — so chasing a document chain (A references B references C) stays within budget.

### D. Code-Enforced Safety Limits (Hard Stops)
The agent loop has hard-coded safety mechanisms that cannot be overridden by the LLM. These are a last-resort protection against the prompt engineering constraints failing:

- **Consecutive Same-Status Limit (3):** The agent loop terminates if the same non-ok status (e.g., `networkFailure`, `toolError`) occurs 3 times consecutively. This prevents wasting time on a path that is consistently failing.
- **Consecutive No-Tool-Call Limit (5):** The agent loop terminates if the LLM produces 5 consecutive `ok` responses without any tool calls. This prevents infinite spinning when the LLM ignores the prompt instructions and keeps generating text instead of calling tools.
- **Context Compression:** The agent automatically compresses the chat history when the context window approaches capacity. This allows long sessions to continue even if the LLM exceeds the 10-call budget.

### E. The "Good Enough" Response (Defensive Hallucination Prevention)
If the knowledge base is incomplete, the LLM is instructed to explicitly state what it found and what it did not find.

- **Rationale:** Without this rule, LLMs exhibit a "completion bias"—they will invent missing details to provide a seemingly complete answer. This rule enforces intellectual honesty. By training the LLM to output *"I found [X] in the docs, but [Y] was not present,"* we ensure the system remains a reliable, factual assistant, even when the underlying knowledge base is deficient.

---

## 6. The "Current Objective" Distinction (Crucial)

The LLM operates on its **Current Objective**, not the user's literal original utterance.

- **Scenario:** User says, *"Read plan.md and execute task 1-4."*
- **Internal Switch:** The LLM reads the plan and sees the third task is *"Refactor auth to use JWT."*
- **RAG Trigger:** When the LLM searches, it searches for *"How to implement JWT in this framework"*—**not** the original *"Read plan.md"* string.
- **Implementation:** The protocol explicitly replaces "user question" with "current active objective" in the system instructions to ensure searches remain semantically relevant to the subtask at hand.

---

## 7. Prompt Engineering Architecture (Layered Instructions)

To optimize token usage and avoid "Lost-in-the-Middle" syndrome, we use a layered prompt approach:

1. **System Prompt (Base Layer):** Contains only the tool definitions and the mandatory trigger (`loadSkill`).
2. **Skill (Dynamic Layer):** Contains the 10-call budget strategy, stage breakdown (stages 0-3), FTS5 syntax warnings, and failure diagnosis scenarios. This is loaded *only* when the RAG system is about to be used, keeping the initial context window lean.

---

## 8. Developer Onboarding Checklist

If you are new to this system, remember these three golden rules:

1. **Do not add a Cross-Encoder reranker.** The agentic loop already handles relevance sorting dynamically via the LLM's reasoning and is far more flexible for multi-document stitching.
2. **Never hard-code a search type.** Always let the LLM diagnose the failure (Noise vs. Zero-Result vs. Conceptual) before pivoting. `queryBestMatch` is the safe default.
3. **Respect the 10-Call Budget.** The skill instructs the LLM to limit itself to 10 tool calls per objective. If the LLM exceeds this (it can, since it's a prompt instruction, not a code limit), it is a sign we need better RAG index quality, not a larger budget. Raising the budget drastically increases latency and context confusion.

---

## 9. Design History: Lessons & Post-Mortems

Each entry follows the same skeleton: **Symptom** (what was observed) → **Root cause** → **Approaches tried (and why they failed)** → **Resolution** (the current design) → **Guard** (what prevents a regression). Full measurements and battery tables are in `rag_report2.md` at the workspace root (one level above this repository).

### L1. "I want file X" — a name query is not a content ranking

**Symptom.** The user asks for a document by name ("want the token spec file, by name"); the agent's searches surface *other* documents. In the eval battery, `f1`–`f3` (target `auth_token_spec.md`): semantic rank 1, `best` and `text` — absent from top-5. `f5`/`f6` work in all three modes; `f4` reaches rank 4 in `best`.

**Root cause** (verified against the committed code and the battery): the name query's own words (`want`, `the`, `file`, `by`, `name`) do not appear in the target's text, and FTS5's raw `MATCH` does not strip stopwords and treats `auth_token_spec` as one token — so the target earns **no FTS score at all**, only its vector score. In `queryBestMatch`'s RRF (`RrfK = 10`, weights 1.0/1.0, pools of `topK × 10`), any chunk that *also* matches FTS earns from both engines. On the 14-chunk eval corpus the vector pool (k=50) covers the whole corpus, so **every** chunk gets a vector rank: the worst possible double score (`1/15 + 1/24 = 0.108`) exceeds the best possible single-engine score (`1/11 = 0.091`). Any FTS match therefore outranks the rank-1 vector-only hit — on small corpora. The five docs that beat the target are exactly the ones that *mention* it. On large corpora this self-heals, because few FTS hits land in the vector pool (see `rag_design2.md` §3.4).

**Approaches tried (and why they failed):**
- *Letting the vector model "see" the file name* — the source prefix (`File: <path> | `) helps when the query names the file in natural language (`f5`/`f6` work), but a bare identifier (`f2`: `auth_token_spec`) still rides on embedding luck, and a document's own text never mentions its own name.
- *Fusion weight tuning (`FtsWeight` 2.0 → 1.0)* — fixed the "mentioning chunks flood top-K" problem on content queries (c4 `best` rank 2 → 1) but did not fix the name queries. The weights are corpus-size-dependent (see L5); the *signal* was wrong.

**Resolution.** Treat the name as *metadata*, not content, and separate the two jobs: `listRAGSources` (listing + substring `filter`) resolves name → exact indexed path; `readRAGSource` (bare-name suffix resolution, bounded by `maxBytes`) reads the whole document; `queryReadFile` also resolves bare names. The `knowledge-retrieval` skill (v1.1.0) routes here: *"When a result mentions another document by name, or the user asks for 'the file X': call `listRAGSources` with a filter to resolve the exact indexed path, then `readRAGSource`."* Verified end-to-end on the committed build: a name query resolved `auth_token_spec.md` and answered correctly in 29 s (2 calls); a cross-reference chain (`auth_overview.md` → `auth_token_spec.md`) was followed and answered correctly in 55 s.

**Guard.** `f1`–`f6` in the eval harness (plus the bare-name read-workflow check), and the semantic `f1 = 1` regression check. Follow-ups: `rag_design2.md` §8 (source-name signal in fusion; FTS underscore/stopword policy).

### L3. BOS/EOS layout split between embedding paths

**Symptom.** The string embedding path applies the model's special tokens (`add_special`) while the `int[]` path did not — indexed and queried embeddings could silently diverge. Measured on the nomic model: cosine 0.995–0.998, zero window drift. Latent, not catastrophic — but the two paths had no obligation to agree.

**Approach tried (and why it failed):** baking the special tokens into the caller's token stream produced the *wrong order* — `[docPrefix, BOS, chunkPrefix, ...]` instead of llama.cpp's `[BOS, docPrefix, chunkPrefix, ...]`.

**Resolution.** The embedder owns the layout: `withSpecials()` in `llama_embedder.d` wraps the `int[]` overloads with `[BOS, prefix, content, EOS]` using ids cached from `llama_vocab_get_add_bos/eos`, and the batch budget reserves `specialCount` up front. All paths now share one layout (`rag_design2.md` §2).

**Guard.** `probe_embed.c` (token-for-token equivalence with `add_special=true`, cosine 1.000000) and `probe_token_drift.c` (budget exactness, drift 0).

### L5. Fusion weights are a band-aid, not a fix

**Symptom.** With `FtsWeight = 2.0`, `best`-mode top-K was flooded with mentioning chunks on some content queries (c4 at rank 2).

**Approach tried:** halving to 1.0 fixed that case (best rank 2 → 1) and is the current value.

**Lesson.** The "right" fusion weight is corpus-size-dependent (L1's arithmetic) — tuning masks missing signals rather than adding them. Prefer the correct signal (L1's name→path architecture) over weight changes. Current values: `VecWeight = FtsWeight = 1.0`, `RrfK = 10`.
