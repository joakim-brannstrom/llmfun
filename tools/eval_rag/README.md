# eval_rag — RAG retrieval evaluation harness

Indexes a small markdown corpus with the **real configured embedder** (local
nomic model by default) into a scratch RAG database and measures the retrieval
quality of the three query paths:

- `semantic` — `RAG.querySemantic`
- `best`     — `RAG.queryBestMatch` (RRF fusion of vector + FTS5)
- `text`     — `RAG.queryTextSearch` (FTS5)

For every query the first chunk belonging to the **expected** source file is
located in the result list and hit@1 / hit@3 / hit@K and MRR are reported, plus
the raw per-file distance ranking from the vec0 table (the pure embedding
signal before fusion/ranking code) and a read-workflow check (bare file name →
whole document read).

## Files

| Path | Purpose |
| ---- | ------- |
| `eval_rag.d` | The harness (dub configuration `rag_eval`). |
| `corpus/` | 14 synthetic markdown documents with cross-references (auth, deploy, storage, etc.). |
| `config.example.yaml` | Minimal readConfig-passing config for the local nomic embedder; copy next to where you run the binary and adjust `modelPath`. |
| `results/` | Archived reference runs (JSON + captured summaries) with the values a new run must reproduce to be comparable - see `results/README.md`. |
| `probe_embed.c` | Raw llama.cpp probe: pooling/norm sanity, special-token (BOS/EOS) A/B measurement, and proof that `llama_tokenize(add_special=true)` matches the token-array mirror wrap (model's `add_bos`/`add_eos` flags) token-for-token (cos = 1.000000). |
| `probe_token_drift.c` | Measures full-string `add_special=true` token counts against prefix + specials + Σ per-word counts (the chunker's counting): delta = 0 across boundary cases and the whole corpus, so the batch budget is exact. |
| `probe_pooling.c` | Prints the GGUF pooling type and a raw embedding cosine matrix. |
| `probe_fts_limit.c` | SQLite FTS5 probe: `row_number() OVER (ORDER BY rank) ... LIMIT` semantics. |
| `probe_ab.c` | sqlite-vec probe: verifies the int8 A/B SQL mechanics end to end on synthetic 8-dim vectors (vec0 metadata readback, `INSERT ... SELECT` with `vec_quantize_int8`, KNN with a pre-quantized blob and with an inline `vec_quantize_int8(vec_f32(?), 'unit')` MATCH rhs). |
| `probe_emb2.c` | llama.cpp probe for the local embedder: pooling type, explicit output flags on every token, and `llama_get_embeddings_seq` / `llama_get_embeddings_ith` null checks with an optional text argument. |

## Build

```
dub build --config=rag_eval        # binary: build/rag_eval
```

## Run

The harness needs a config (any readConfig-passing yaml - use
`config.example.yaml` as the template) and the embedder gguf. Copy the config
next to where you run the binary and adjust `modelPath` (the gguf must match
the `dimensions` and prefix entries):

```
# from the llmfun checkout root (the directory containing dub.sdl)
cp tools/eval_rag/config.example.yaml eval_rag.yaml   # adjust modelPath
dub build --config=rag_eval                           # binary: build/rag_eval
LD_LIBRARY_PATH=build ./build/rag_eval run \
    --corpus tools/eval_rag/corpus \
    --db /tmp/eval.sqlite3 \
    --config eval_rag.yaml \
    --topK 5 \
    --out /tmp/eval.json
```

Flags: `--keep-db` (reuse an existing DB; re-indexing is skipped because
`add` dedups on content hash), `--out` (JSON report), `--topK`.

## int8 A/B mode

Compares int8-quantized KNN against the float32 KNN on the same corpus and
queries, as the adoption gate for int8 embeddings (see plan/appendix.md).
The harness builds an int8 mirror table with
`INSERT INTO mirror SELECT id, sourceId, vec_quantize_int8(embedding, 'unit')
FROM <float table>`, generates query cases from the corpus (doc-comment first
line for `.d`, first non-heading line for `.md`; the float32 KNN result is the
reference), and reports recall@1/3/5/K (id-set overlap against float32),
hit@1/3/K and MRR for both rankers, the float-hit-but-int8-miss count, and the
mean KNN time per ranker.

```
LD_LIBRARY_PATH=build ./build/rag_eval ab \
    --corpus tools/eval_rag/corpus \
    --db /tmp/eval_ab.sqlite3 \
    --config eval_rag.yaml \
    --topK 10 \
    --out /tmp/eval_ab.json
```

Reproducibility: a run is only comparable to the archived references in
`results/` with the same embedder gguf, the same corpus, and `--topK 10` -
see `results/README.md` for the exact reference values. Timing columns are
host-specific; recall/hit/MRR are stable.

Scale-out options: `--sources <dir>` indexes additional source trees
recursively (repeatable), `--ext d,md` filters by extension, `--auto-queries N`
takes every Nth indexed file and generates a query case from it, e.g.:

```
LD_LIBRARY_PATH=build ./build/rag_eval ab \
    --corpus tools/eval_rag/corpus \
    --sources source/llm/rag \
    --ext d \
    --auto-queries 20 \
    --db /tmp/eval_ab.sqlite3 \
    --config eval_rag.yaml \
    --topK 10
```

A full `--sources source` run stalled on the CPU embedder (mid-indexing, no
error output, cause not diagnosed); the `source/llm/rag` subset (465
chunks) completed in ~7 min on CPU. Results as of 2026-11-13: recall@1
0.94, zero float-hit-but-int8-miss cases, int8 KNN 2-4x faster.

`vec_f32` (not `vec_float32`) converts blobs to float vectors in this
sqlite-vec version, and a vec0 table declaring `id INTEGER PRIMARY KEY` has
no `rowid` column - use `id`.

## Probe builds

From the llmfun checkout root (the sqlite3.o and sqlite-vec.o objects are
produced by the sqlite3.mak build):

```
gcc -O1 -I vendor/sqlite3 -o probe_fts_limit tools/eval_rag/probe_fts_limit.c \
    build/sqlite3.o -lpthread -ldl -lm
gcc -O1 -I vendor/llama.cpp/include -I vendor/llama.cpp/ggml/include \
    -o probe_embed tools/eval_rag/probe_embed.c -L build -lllama -lggml -lggml-base -lggml-cpu -lm
gcc -O1 -I vendor/llama.cpp/include -I vendor/llama.cpp/ggml/include \
    -o probe_token_drift tools/eval_rag/probe_token_drift.c -L build -lllama -lggml -lggml-base -lggml-cpu -lm
gcc -O1 -DSQLITE_CORE -I vendor/sqlite3 -I vendor/sqlite3-vec \
    -o probe_ab tools/eval_rag/probe_ab.c build/sqlite3.o build/sqlite-vec.o -lpthread -ldl -lm
gcc -O1 -I vendor/llama.cpp/include -I vendor/llama.cpp/ggml/include \
    -o probe_emb2 tools/eval_rag/probe_emb2.c -L build -lllama -lggml -lggml-base -lggml-cpu -lm
LD_LIBRARY_PATH=build ./probe_embed nomic-embed-text-v2-moe.Q8_0.gguf
LD_LIBRARY_PATH=build ./probe_token_drift nomic-embed-text-v2-moe.Q8_0.gguf tools/eval_rag/corpus
LD_LIBRARY_PATH=build ./probe_pooling nomic-embed-text-v2-moe.Q8_0.gguf
LD_LIBRARY_PATH=build ./probe_emb2 nomic-embed-text-v2-moe.Q8_0.gguf
```

## Baseline results (nomic-embed-text-v2-moe, topK=5, corpus as of 2026-09)

| configuration | semantic hit@1 | semantic MRR | best hit@1 | best MRR |
| ------------- | -------------- | ------------ | ---------- | -------- |
| before BOS/EOS fix | 0.44 | 0.656 | 0.50 | 0.573 |
| after BOS/EOS fix  | 0.81 | 0.896 | 0.44 | 0.609 |
| + equal RRF weights (final) | 0.81 | 0.896 | 0.50 | 0.641 |
| + L2-normalization (rejected) | 0.75 | 0.865 | 0.44 | 0.609 |

`text` search is unaffected by embedder changes (hit@1 = 0.31, MRR = 0.427).

The read-workflow phase (`readSource` + bare-name `queryReadFile`) passes for
all tested files: full document read is byte-exact against the corpus file.
