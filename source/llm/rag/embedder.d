// Embedding backend abstraction for RAG.
// Provides an interface that decouples RAG from specific model implementations,
// allowing both local (llama.cpp) and remote (HTTP API) backends.

module llm.rag.embedder;

import std.array : empty;
import std.sumtype : SumType, match;

import llm.config : EmbedConfig, LocalEmbedConfig, RemoteEmbedConfig;
import llm.rag.embedder_http;
public import llm.query : HttpError;

alias EmbedResult = SumType!(float[], HttpError, string);

/// Produce an embedding vector for the given text.
/// Returns float[] of fixed dimension (e.g. 384, 768, 1536).
/// Throws Exception on failure (model error, network error).
interface Embedder {
    /// Produce an embedding vector for the given text.
    EmbedResult embed(string text);

    /// Maximum number of tokens that can be processed in one batch.
    /// Used by RAG.add() to determine chunk size.
    int batchSize();

    /// Destroy all critical resources.
    void destroy();
}

/// Factory function to create an Embedder from an EmbedConfig sum type.
Embedder createEmbedder(EmbedConfig config) {
    import llm.rag.embedder_llama;

    return config.match!((LocalEmbedConfig local) {
        version (llmfun_llama_backend) {
            import llm.llama.model : Model, LlamaParams, contextEmbedding;

            auto params = LlamaParams.make();
            params = contextEmbedding(params, cast(uint) local.nBatch);
            params.ctx.n_ctx = cast(uint) local.context;
            auto model = new Model(local.modelPath, params);
            Embedder e = new LlamaEmbedder(model);
            return e;
        } else {
            throw new Exception("llmfun_llama_backend not compiled");
            return null;
        }
    }, (RemoteEmbedConfig remote) {
        Embedder e = new RemoteEmbedder(remote);
        return e;
    });
}
