module llm.common.embedder;

import std.sumtype : SumType, match;

import llm.common.config;

struct EmbedError {
    string errorMsg;
}

alias EmbedResult = SumType!(float[], EmbedError);

/// Produce an embedding vector for the given text.
/// Returns float[] of fixed dimension (e.g. 384, 768, 1536).
/// Throws Exception on failure (model error, network error).
interface Embedder {
    /// Name of the model used for creating embeddings
    string modelName();

    /// Dimensions of the vectors created.
    long dimensions();

    /// Produce an embedding vector for the given text.
    EmbedResult embed(string text);

    bool supportsTokenization();

    /// Produce an embedding vector for the given tokens.
    /// Only available if supportsTokenization is true.
    EmbedResult embed(int[] tokens);

    /// Only available if supportsTokenization is true.
    int[] tokenize(string text);

    /// Only available if supportsTokenization is true.
    string detokenize(int[] tokens);

    /// Maximum number of tokens that can be processed in one batch.
    /// Used by RAG.add() to determine chunk size.
    int batchSize();

    /// Destroy all critical resources.
    void destroy();
}

/// Signature for embedder factory functions.
/// Registered by backend modules at startup and called by createEmbedder().
alias EmbedderFactory = Embedder function(EmbedConfig config);

/// Registry of embedder factories indexed by config type key.
private shared EmbedderFactory[string] _factories;

/// Return the type name for the type of embedder.
private string configTypeKey(EmbedConfig config) {
    return config.match!((RemoteEmbedConfig _) => "remote", (LocalEmbedConfig _) => "local",);
}

/// Register an embedder factory for the given config type.
/// Called from shared static this() in backend modules.
void registerEmbedderFactory(string typeKey, EmbedderFactory factory) {
    _factories[typeKey] = factory;
}

/// Factory function to create an Embedder from an EmbedConfig sum type.
Embedder createEmbedder(EmbedConfig config) {
    auto key = configTypeKey(config);

    auto pFactory = key in _factories;
    auto f = pFactory ? *pFactory : null;

    if (f) {
        return f(config);
    }

    return null;
}
