results/ - archived reference runs (JSON report + captured summaries). A new
run is only "comparable" to these if it uses the same embedder (nomic-v2-moe,
Q8_0 gguf, 768 dims, cpu), the same corpus, and --topK 10. Timing columns are
host-specific; only the recall/hit/MRR columns are stable across hosts.

Known reference values (2026-11-13, nomic-embed-text-v2-moe Q8_0, topK 10):

corpus only (ab_reference_2026-11-13_nomic-v2-moe_Q8_0.json):
  recall@1 0.938  recall@3 0.979  recall@5 0.988  recall@10 0.994
  float: hit@1 13/16 hit@3 16/16 hit@K 16/16 MRR 0.896
  int8:  hit@1 12/16 hit@3 16/16 hit@K 16/16 MRR 0.865
  float-hit but int8-miss: 0/16
  mean knn ms: float ~1.1, int8 ~0.3 (host-specific)

corpus + llmfun/source/llm/rag sources (ab_sources_rag_2026-11-13_summary.txt):
  recall@1 0.941  recall@3 0.941  recall@5 0.953  recall@10 0.912
  float: hit@1 13/17 hit@3 16/17 hit@K 16/17 MRR 0.843
  int8:  hit@1 12/17 hit@3 16/17 hit@K 16/17 MRR 0.814
  float-hit but int8-miss: 0/17
