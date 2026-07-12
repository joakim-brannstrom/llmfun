# Design Document: Agentic RAG Retrieval Loop

---

## 1. Overview & Purpose

This document describes the architecture and operational logic of our RAG (Retrieval-Augmented Generation) pipeline. Unlike traditional RAG systems that rely on a static retriever + cross-encoder reranker, our system utilizes an **Agentic Retrieval Loop**.

The LLM acts as an autonomous researcher, equipped with multiple search tools, to actively hunt for, read, and verify information from a corpus of knowledge bases (databases). The system is designed to handle complex, multi-step coding and technical queries where the relevant information might span multiple files or require follow-up "chunk chasing."

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

**Trade-off:** We trade *predictable latency* (fixed ~500ms for a reranker) for *higher accuracy*. To prevent infinite loops, we enforce a strict Hard Cap on tool calls and implement a "Partial Answer" safety valve.

*(Note: While we don't have a Cross-Encoder, the LLM's internal reasoning effectively acts as a dynamic, multi-pass reranker, discarding irrelevant chunks as it reads them).*

---

## 3. Architecture: Tool Suite

The system exposes five core tools to the LLM. All search tools support a `database` parameter (scoping) or `"*"` (global).

| Tool | Function | Use Case | Key Constraint |
| :--- | :--- | :--- | :--- |
| **`listRAGDatabases`** | Discovers available DB names. | **Discovery.** Called once per objective to map the terrain. | Must be called before scoping searches. |
| **`queryBestMatch`** | RRF (Reciprocal Rank Fusion) merging FTS5 + Vector. | **The 80% Default.** Balances keyword matching with conceptual meaning. Handles poorly crafted queries gracefully. | Returns a mixed bag; can be noisy on wide scopes (`"*"`). |
| **`querySemantic`** | Vector/Embedding search. | **Conceptual broad strokes.** Use when objective lacks specific nouns, or when FTS5 fails. | Fast, but misses exact function names. |
| **`queryTextSearch`** | FTS5 (Full-Text Search) with implicit `AND`. | **Precision.** Use only when you possess a highly unique, non-generic keyword (e.g., `authenticate_user_v2`). | **Warning:** Generic terms (e.g., "user", "login") cause implicit `AND` to return zero or flood results. |
| **`queryReadFile`** | Exact line lookup in the index. | **Chunk Chasing.** Grabs the exact raw text of a specific line. | **Only takes a single `lineNumber`.** No ranges. |

---

## 4. Design Rationale: Why the Retrieval Strategy Is Structured This Way

The retrieval strategy is engineered to mitigate the specific cognitive weaknesses of LLMs—particularly poor keyword extraction, suboptimal query planning, and a tendency toward "random walk" behavior—while exploiting their strengths in contextual synthesis and iterative reasoning.

### A. Parallel Discovery Over Sequential Guessing
The protocol mandates that `queryBestMatch` and `listRAGDatabases` be executed simultaneously on the first turn.

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

---

## 5. Design Rationale: Safety Mechanisms and Cost Control

The safety mechanisms are not arbitrary constraints; they are calibrated responses to specific operational risks, latency requirements, and the inherent brittleness of the FTS5 search syntax.

### A. The 10-Call Hard Cap (Calibrated Calculus)
The cap is set at **10**, not 5, 8, or 20.

- **Rationale (The Calculus):** Internal telemetry indicates that approximately 50% of all FTS5-based searches return zero results due to the implicit `AND` issue. A cap of 5 would leave the LLM with only 2 to 3 successful reads—insufficient for complex coding queries. A cap of 20 would push latency beyond acceptable thresholds (often exceeding 45-60 seconds) and bloat the context window with failed searches, confusing the model. 
- **The Sweet Spot (10):** With 10 calls, the LLM can afford 2 discovery probes, 3 to 4 targeted searches (absorbing the 50% failure rate), 2 to 3 individual line reads (`queryReadFile`), and 2 verification calls. This keeps total execution time under approximately 25-30 seconds while providing enough runway to dig through fragmented documentation.

### B. The Confidence Check (Forcing a "Shift" from Discovery to Verification)
The protocol mandates that if, after **6 calls**, the LLM does not have a complete answer, it must stop generating new search strategies and reserve the remaining 4 calls exclusively for verification.

- **Rationale:** The law of diminishing returns applies sharply to RAG retrieval. If the core answer hasn't been found in the first 6 attempts, it is unlikely to be found in the 7th or 8th. Continuing to search at that point merely delays the inevitable. By forcing a shift to verification, we change the failure mode. Instead of timing out with an empty context, the LLM uses the final calls to validate the partial evidence it *does* have. This guarantees that even a "failed" retrieval results in a defensible, partially informed answer rather than a hallucination.

### C. The Single-Line Constraint (Driving the Cap Upwards)
`queryReadFile` takes only a single `lineNumber`. This architectural constraint directly influences why the cap is 10 rather than a lower number.

- **Rationale:** Because the system cannot read a range of lines in one call, reading three specific lines costs three separate tool calls. If the cap were 5, the LLM could only read two lines before running out of calls. The 10-cap intentionally allocates 3-4 calls specifically for line reads, allowing the LLM to chase adjacent code blocks and verify multiple sources without cannibalizing its search budget.

### D. The "Good Enough" Response (Defensive Hallucination Prevention)
If the knowledge base is incomplete, the LLM is instructed to explicitly state what it found and what it did not find.

- **Rationale:** Without this rule, LLMs exhibit a "completion bias"—they will invent missing details to provide a seemingly complete answer. This rule enforces intellectual honesty. By training the LLM to output *"I found [X] in the docs, but [Y] was not present,"* we ensure the system remains a reliable, factual assistant, even when the underlying knowledge base is deficient.

---

## 6. The "Current Objective" Distinction (Crucial)

The LLM operates on its **Current Objective**, not the user's literal original utterance.

- **Scenario:** User says, *"Read plan.md and execute task 1-4."*
- **Internal Switch:** The LLM reads the plan and sees Task 3 is *"Refactor auth to use JWT."*
- **RAG Trigger:** When the LLM searches, it searches for *"How to implement JWT in this framework"*—**not** the original *"Read plan.md"* string.
- **Implementation:** The protocol explicitly replaces "user question" with "current active objective" in the system instructions to ensure searches remain semantically relevant to the subtask at hand.

---

## 7. Prompt Engineering Architecture (Layered Instructions)

To optimize token usage and avoid "Lost-in-the-Middle" syndrome, we use a layered prompt approach:

1. **System Prompt (Base Layer):** Contains only the tool definitions, the mandatory trigger (`getThinkingTemplate`), and the strict Hard Cap count.
2. **Thinking Template (Dynamic Layer):** Contains the full 10-call strategy, FTS5 warnings, and failure diagnosis charts. This is loaded *only* when the RAG system is about to be used, keeping the initial context window lean.

---

## 8. Developer Onboarding Checklist

If you are new to this system, remember these three golden rules:

1. **Do not add a Cross-Encoder reranker.** The agentic loop already handles relevance sorting dynamically via the LLM's reasoning and is far more flexible for multi-document stitching.
2. **Never hard-code a search type.** Always let the LLM diagnose the failure (Noise vs. Zero-Result vs. Conceptual) before pivoting. `queryBestMatch` is the safe default.
3. **Respect the 10-Cap.** If the LLM exceeds this, it is a sign we need better RAG index quality, not a larger budget. Raising the cap drastically increases latency and context confusion.
