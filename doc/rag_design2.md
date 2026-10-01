# Design Document: RAG Indexing, Embedding & Fusion Pipeline

Companion to `rag_design.md` (the agent-facing retrieval loop). This document covers what happens *inside* the pipeline: how documents are chunked and embedded, how the three query paths rank, and how names resolve to documents. Constants quoted below are the committed values in `source/llm/rag/database.d` and `source/llm/rag/rag.d`; `doc/database.md` documents the SQL-level details (FTS5 syntax, RRF, multi-database behavior) and is the reference for anything not repeated here.

---

## 1. Indexing Pipeline

### 1.1 Dual chunkers

`RAG.add` dispatches on the embedder's capabilities:

- `embedder.supportsTokenization` → **token-exact chunking** (`runOnTokens`)
- otherwise → **grapheme chunking** (`runOnText`)

### 1.2 Token-exact chunking (default with llama.cpp)

The window is built in *tokens*, never in characters, so what is sent to the embedder is exactly what the budget allows — no re-tokenization can happen after windowing, and there is no drift between "what I thought I sent" and "what the model saw" (verified: `probe_token_drift.c` reports zero drift).

Mechanics:

- The document is tokenized word-at-a-time with `addSpecial: false`.
- The configured document prefix (config `documentPrefix`, default `"search_document: "`) is tokenized once and hoisted — it is prepended to every window's token stream.
- Window size = `batchSize - prefixTokens - specialCount`, where `specialCount` is the number of special tokens the model adds (`max(1, tokenize("a", addSpecial: true).length) - 1`). This capacity cap is what guarantees the final embedding never exceeds the model's batch.
- Each window is embedded via the `int[]` overload with layout `[BOS, prefixTokens, window, EOS]` (see §2).
- A chunk that the model still rejects is logged with `warningf` ("chunk DROPPED") instead of vanishing silently — see Lesson L4 in `rag_design.md`.

### 1.3 Grapheme chunking (fallback path)

For embedders without tokenization, windows are built over graphemes (chunk size budget in graphemes) and embedded through the string path. On rejection the window is *halved* and retried (bounded iteration count), and the batch size adapts up/down. This halving fallback exists **only** in this path; the token path's protection is the capacity-capped budget plus the drop warning.

### 1.4 Deduplication

`add` is a no-op when the source's content hash matches an existing index: chunks, embeddings, and FTS rows are left untouched, so re-adding unchanged content cannot corrupt or bloat an index.

---

## 2. Embedding Layout Contract

Every embedding in the system — indexed or queried, string path or int[] path, with or without a source prefix — has the same token layout:

```
[BOS, modelPrefix, (sourcePrefix), content, EOS]
```

- `modelPrefix` — from config (`queryPrefix` = `"search_query: "`, `documentPrefix` = `"search_document: "`).
- `sourcePrefix` — `Topic: <name> | ` / `Url: <url> | ` / `File: <path> | `, **index path only**, never on queries. Only the *embedding* carries it; the stored chunk text and the FTS5 index do not. This is what makes "what does file X say about retries" work semantically (see `rag_design.md` §3.1).
- `BOS`/`EOS` — appended by the embedder, not the caller. `withSpecials()` in `llama_embedder.d` wraps the `int[]` overloads so both the string path (`add_special` applied inside llama.cpp) and the token path produce the identical layout. The special token ids are cached once at construction via `llama_vocab_get_add_bos/bos` / `get_add_eos/eos`.

Why this matters: the two historical failure modes were (a) the int[] path silently lacking BOS/EOS, and (b) an attempt to bake specials into the caller's token stream, which produced the *wrong order* (`[docPrefix, BOS, chunkPrefix, ...]` instead of `[BOS, docPrefix, chunkPrefix, ...]`). Measured impact on the nomic model was small (cosine 0.995–0.998) but real, and it made index/query layouts diverge. The embedder-side wrapper makes the layout a single responsibility of the embedder. `probe_embed.c` verifies token-for-token equivalence against `add_special=true` (cosine 1.000000).

---

## 3. Query Paths

All three take a `database` scope (`"*"` = all) and run across the matched databases in parallel. Constants: `RrfPoolMultiplier = 10`, `RrfK = 10`, `VecWeight = 1.0`, `FtsWeight = 1.0`, `MaxFromSource = 2`.

### 3.1 `querySemantic` — vector KNN

- SQL: `embedding MATCH :embedding AND k = :limit`, ranked by `row_number() OVER (ORDER BY distance)`; `k = topK` (no pool multiplier — the KNN pool *is* the answer).
- D-side: per-database results are merged with `randomizeRanks` (content-hash-seeded tie shuffle, see §4), stable-sorted by rank ascending (lower distance = better), then `takePerSource(topK, 2)`.

### 3.2 `queryTextSearch` — FTS5

- SQL: `WHERE FtsChunksTbl MATCH :query ORDER BY rank LIMIT :limit`. `rank` is bm25 (more negative = better).
- The query string is passed **raw** to FTS5 `MATCH`: space-separated terms are implicit `AND`, and FTS5's unicode61 tokenizer does *not* strip stopwords (`the`, `by`, `want` all participate) and treats `auth_token_spec` as one token. This is why generic queries return zero and why name queries fail in text mode (Lesson L1). FTS5 query syntax (`AND`/`OR`/`NOT`, prefix, `NEAR`, groups) is documented in `doc/database.md`.

### 3.3 `queryBestMatch` — RRF fusion

- Both legs pull a **pool**, not the final answer: vector `k = topK * 10`, FTS `LIMIT topK * 10`.
- RRF score per chunk: `1.0 * 1/(10 + coalesce(vecRank, 1000)) + 1.0 * 1/(10 + coalesce(ftsRank, 1000))` — a chunk missing from one leg simply contributes nothing from it.
- SQL returns the pool ordered by fusion score descending, truncated to the pool size.
- D-side: `randomizeRanks` seeded by the *text* query, stable sort by score descending, `takePerSource(topK, 2)`.
- **Fallback:** if the query embedding is empty, `queryBestMatch` degrades to `queryTextSearch` (logged at trace).

### 3.4 The small-corpus double-dip property

Because the vector pool is 10×topK, on a corpus smaller than that pool **every** chunk gets a vector rank, and any chunk that *also* matches FTS earns score from both engines while a content-miss-only vector hit earns from one. With topK=5 on a 14-chunk corpus: the best single-engine score is `1/11 = 0.091`, while the *worst* possible double score (`1/15 + 1/24`) is `0.108` — so **any FTS match outranks the rank-1 vector-only hit**. This is not a bug; it self-heals on large corpora where few FTS hits land in the vector pool. It does mean that on small, densely cross-referencing corpora, `queryBestMatch` systematically prefers "mentioning" documents over the named document itself — the direct motivation for the name-resolution tools in section 5 (Lesson L1 has the full arithmetic and the `f`-family evidence).

---

## 4. Determinism

Identical queries return identical top-K, enforced in code (`rag_design.md` §4E): the tie-breaking shuffle is seeded with a content hash of the query text (seed differs per query, so it still breaks database-order bias among equal ranks), and the subsequent rank sort is stable. This makes the index a stable function of the query and makes retrieval bugs reproducible — a property the evaluation harness (§7) depends on.

---

## 5. Name Resolution Architecture

Content ranking cannot reliably answer "give me file X": a document's own text rarely contains its own name, and the query's natural-language words (`want the file ... by name`) rarely appear in the target either. Names are *metadata*; matching them is a listing problem, not a ranking problem.

The design therefore separates the two:

1. **`listRAGSources`** — lists indexed documents (path/topic/URL, chunk counts) with an optional substring `filter`. This resolves *name → exact indexed path*. It is deliberately not a search.
2. **`readRAGSource`** — reads a whole document reconstructed from its chunks; resolves bare file names by path suffix; bounded by `maxBytes` (default 64 KiB, truncation reported).
3. **`queryReadFile`** — exact line lookup; also resolves bare names, for the chunk-chasing use case.

The `knowledge-retrieval` skill (v1.1.0) routes the agent here: *"When a result mentions another document by name, or the user asks for 'the file X': call `listRAGSources` with a filter to resolve the exact indexed path, then `readRAGSource`."*

End-to-end verification (committed build): a name query resolved `auth_token_spec.md` and answered correctly in 29 s (2 calls); a cross-reference chain (`auth_overview.md` → `auth_token_spec.md`) was followed and answered correctly in 55 s.

Known limitation: suffix matching has no path-boundary check (`xauth_token_spec.md` would also match `auth_token_spec.md`). Acceptable for now; flagged in §8.

---

## 6. Index Integrity & Migration Invariants

- **A change to the embedding layout, the chunker, or the prefix logic invalidates every existing database.** Old chunks keep their old embeddings and old texts. Re-index after such a change — the A/B demo for the `ref` bug (Lesson L2) used separate databases for this exact reason.
- **A dropped chunk is a warning, not a state.** The index does not record "this document is incomplete"; the warning log is the only signal. If warnings appear, re-index with a model/config whose budget fits.
- **`add` dedup is content-hash based** (§1.4): unchanged content is never re-embedded, changed content replaces the old rows.

---

## 7. Evaluation Harness

`tools/eval_rag/` (`eval_rag.d`, auto-indexes a fresh DB or reuses with `--keep-db`, `add`-dedup makes reruns idempotent):

```
./llmfun/build/rag_eval run \
  --corpus llmfun/tools/eval_rag/corpus \
  --db /workarea/evals/eval_X.sqlite3 \
  --config /workarea/.llmfun.yaml \
  --topK 5 --out /workarea/evals/eval_X.json
```

**Corpus:** 14 small markdown files, single-window each, with deliberate cross-references (overview → spec, changelog → files, incident → runbook).
**Query families:** `f1`–`f6` name queries (bare name, name + extension, natural phrasing), `c1`–`c6` content queries, `x1`–`x4` cross-reference queries. Each case runs in three modes — `semantic`, `best`, `text` — and the JSON records the final top-5 (`top`), the rank of the expected document (`rank`, `-1` = absent), and the pure vector-distance top-3 (`rawTop`) so ranking problems can be decomposed per engine.

**Current baseline** (committed code, topK=5, deterministic across re-indexes):

| mode | hit@1 | hit@3 | hit@K | MRR |
| :--- | :--- | :--- | :--- | :--- |
| semantic | 15/16 | 16/16 | 16/16 | 0.969 |
| best | 9/16 | 12/16 | 13/16 | 0.672 |
| text | 5/16 | 9/16 | 9/16 | 0.427 |

Name-family detail (semantic / best / text):

| case | target | ranks |
| :--- | :--- | :--- |
| f1–f3 | `auth_token_spec.md` | 1 / -1 / -1 |
| f4 | `deploy_runbook.md` | 1 / 4 / -1 |
| f5 | `db_schema.md` | 1 / 1 / 1 |
| f6 | `onboarding.md` | 1 / 1 / 2 |

The split is the whole story of Lesson L1: vectors find every named file at rank 1; FTS finds none whose text lacks the query's words; fusion, on this small corpus, prefers the double-matched mentioning docs (f5/f6 survive because their query words *do* appear in the target's own text).

**Probes** (standalone, in the same directory): `probe_embed.c` (layout equivalence vs `add_special`, cosine 1.000000), `probe_token_drift.c` (window budget exactness, drift 0), `probe_pooling.c` (pooling behavior), `probe_fts_limit.c` (FTS `LIMIT` semantics).

**Gap:** the corpus is all single-window files, so multi-window index corruption (Lesson L2) cannot be caught by the battery. Add a long multi-window document with a per-window unique marker word and assert disjoint occurrence counts.

---

## 8. Known Gaps & Future Work

1. **`f`-family in `best`/`text` modes on small corpora** — the double-dip property (§3.4) is inherent to RRF with a pool larger than the corpus. Candidates: a source-name signal in the fusion score (the query can be checked against indexed source names, which are metadata, not content), or an FTS tokenizer policy (underscore splitting, stopword handling) so `auth_token_spec` matches `auth AND token AND spec`.
2. **Path-suffix matching has no boundary check** in `readRAGSource` / `queryByPathAndLine` — a query could match a longer path ending in the same string.
3. **Multi-window coverage** missing from the eval corpus (see §7).
4. **FTS raw `MATCH`**: stopwords and identifiers are not normalized; this is documented behavior, not a bug, but it shapes query strategy (hence the FTS restriction in the skill).
5. **Small-corpus calibration:** the battery is a regression instrument, not an absolute quality measure — absolute numbers will move when the corpus grows, and that is expected.
