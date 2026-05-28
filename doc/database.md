# Design of Best Match

This describe the algorithm for best match used in the database.

FTS5 is a text search algorithm while sqlite-vec is a semanit search. Both have their pro/con. By combining them the result is hopefully better. If there is an exact match then FTS5 find it but also high ranking semantic matches are part of the top-K result. To combine them and avoid e.g. duplication Reciprocal Rank Fusion (RRF) is used.

| Feature           | sqlite-vec (Semantic)                                 | FTS5 (Full-Text Search)                       |
| ----------------- | ----------------------------------------------------- | --------------------------------------------- |
| Goal              | Find concepts and meaning                             | Match exact keywords and phrases              |
| Best for Queries  | Paraphrasing, fuzzy language, cross-lingual queries   | Product codes, names, dates, specific jargon  |
| Explainability	| Low (a "black box" result)	                        | High (based on term frequency)                |
| Query Syntax	    | A vector (numerical list)	                            | Boolean operators, wildcards, etc.            |
| Performance	    | Fast on GPUs; can be slow with brute-force	        | Very fast for keyword lookup                  |
| Maturity	        | Relatively new, community effort is active	        | Mature, built-in, and stable                  |

# How the RRF Formula Works

The core idea:
A document’s final score = sum of its reciprocal ranks from each engine, shifted by a constant.

Formula:
```
score(doc) =   1 / (k + rank_vec(doc)) + 1 / (k + rank_fts(doc))
```

- `rank_vec(doc)` = the position of the document in the vector search results (1 = best).
- `rank_fts(doc)` = the position of the document in the FTS results.
- k is a constant (often 60) that dampens the influence of very high ranks and prevents the score from exploding for rank 1.

Documents that appear high in both lists get the largest scores.
Documents that appear in only one list get a contribution from that list only, but the missing engine is handled by coalesce.

# The Role of coalesce (Handling Missing Documents)

When you LEFT JOIN the two match sets, a document might exist in one but not the other. In SQL, the missing `rank_number` becomes NULL.

The coalesce(`rank_number`, 1000) replaces that NULL with a large placeholder value (here 1000).

Why 1000? Because:

- 1 / (60 + 1000) ≈ 0.00094 – a very small contribution.
- This essentially penalises documents that don’t appear in one engine, but it doesn’t exclude them entirely.
- A document that is rank 1 in vector search but completely missing from FTS will still get a reasonable score:
    `1/(60+1) + 1/(60+1000) ≈ 0.0164 + 0.00094 = 0.0173`.
    This ensures it can still appear in the final results, especially if it’s a top result in one engine.

Without coalesce, the NULL would make the entire expression NULL, and the row would be dropped or sorted unpredictably. The placeholder effectively says: “Treat missing as very low relevance, but keep it in the race.”

# Why RRF is Better Than “Half the Results from Each”

Taking, say, LIMIT/2 from vector and LIMIT/2 from FTS (then stacking or interleaving) has major flaws:

- Overlap is ignored – The same document could appear in both halves, wasting slots on duplicates while missing other high-quality results.
    - RRF starts with the full top-K from both engines, then de-duplicates and re-ranks. No slot is wasted.
- Arbitrary cutoff discards strong candidates – A document ranked 11th in vector search (just below the halfway line) might be 1st in FTS. A LIMIT/2 would drop it from the vector set entirely, and you’d never know it’s an excellent hybrid match.
    - RRF considers all retrieved documents. Even if it’s rank 50 in one list, its combined rank can push it to the top.
- No score normalization – Vector distances and FTS BM25 scores are on completely different scales. Simple mixing doesn’t produce a meaningful unified ranking.
    - RRF only uses ordinal ranks, not raw scores. Ranks are unitless and directly comparable, making fusion trivial and robust.
- Engine strength varies – For a query, vector might be excellent and FTS poor (or vice versa). A rigid 50/50 split forces a balance that might be wrong.
    - RRF automatically lets the stronger engine’s top results dominate, because they’ll get higher reciprocal weights.

Example:

- Document A: Rank 1 in vector, Rank 50 in FTS.
- Document B: Rank 6 in vector, Rank 6 in FTS.

A LIMIT/2 approach that takes top 5 from each would discard Document A from FTS (since it’s rank 50) and might still include it from vector. But it misses the fact that A is excellent overall.
RRF score for A: 1/(60+1) + 1/(60+50) ≈ 0.0164 + 0.0091 = 0.0255
RRF score for B: 1/(60+6) + 1/(60+6) ≈ 0.0152 + 0.0152 = 0.0304
B ends up ranked higher, which is sensible because it’s strong in both engines. A would still appear high enough, though, because its vector rank is stellar.

# Why This Specific Algorithm (RRF)

- No training required – works out of the box, even when engines are completely different (BM25 vs. vector cosine).
- Simple SQL implementation – the formula is just arithmetic; no complex machine learning.
- Robust – handles missing documents gracefully via the coalesce placeholder.
- Constant k (60) acts as a tuning knob: lower k makes top ranks more dominant; higher k flattens the influence. 60 is a common default that balances well.

In short, RRF is a fair, data‑driven way to marry the precision of keyword search with the semantic intuition of vector search, without throwing away information or forcing an arbitrary split.
