// Llama.cpp based embedder implementation.

module llm.rag.embedder_llama;

import logger = std.logger;
import std.string : toStringz;

import llm.rag.embedder;

version (llm_fun_llama_backend) {
    import llm.llama.llama_import;
    import llm.llama.model;
    import llm.llama.util;

    /// Wraps a llama.cpp Model and exposes it through the Embedder interface.
    class LlamaEmbedder : Embedder {
        private {
            Model model;
            llama_token[] smallTokens;
        }

        /// Create a LlamaEmbedder wrapping the given Model.
        this(Model model) {
            this.model = model;
            this.smallTokens = new llama_token[128];
        }

        override void destroy() {
            this.model.destroy;
        }

        /// Produce an embedding vector for the given text via llama.cpp.
        override EmbedResult embed(string text) {
            auto tokens = this.tokenize(text);
            auto batch = tokens.toBatch;
            if (!encode(model, batch)) {
                logger.trace("Failed encoding of tokens in LlamaEmbedder");
                EmbedResult("Failed to encode tokens for embedding");
            }
            return EmbedResult(getEmbedding(model, 0).value);
        }

        // /// Tokenize text using the model's vocabulary.
        // override int[] tokenize(string text) {
        //     return cast(int[]) llm.llama.model.tokenize(model, text, add: false,
        //             tokensBuf: smallTokens);
        // }
        //
        // /// Convert tokens back to text using the model's vocabulary.
        // override string detokenize(int[] tokens) {
        //     return llm.llama.model.detokenize(model, cast(llama_token[]) tokens);
        // }

        /// Return the batch size configured on the llama context.
        override int batchSize() {
            return cast(int) llama_n_batch(model.ctx);
        }
    }
}
