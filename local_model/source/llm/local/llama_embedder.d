/**
 * LlamaEmbedder — wraps a llama.cpp Model for local embedding.
 *
 * This class provides the tokenization and embedding functionality
 * around a loaded Model instance. It uses a two-pass tokenization
 * strategy with a pre-allocated small buffer to avoid heap allocation
 * for short texts (configurable via constructor parameter, default
 * heuristic based on context size).
 *
 * A reusable batch structure is pre-allocated during construction to
 * avoid re-allocating token batches on every embed() call.
 *
 * Implements the Embedder interface from llm.rag.embedder.
 *
 * Note: This class is NOT thread-safe. Concurrent calls to methods
 * from different threads while another thread calls destroy() may
 * result in a data race on the _destroyed flag.
 *
 * License: MPL-2.0
 */
module llm.local.llama_embedder;

import llm.llama.model;
import std.algorithm : max, min;
import std.array : appender;
import std.conv : to;
import std.sumtype : SumType;

import llm.llama.util;
import llm.common.embedder : Embedder, EmbedResult, EmbedError;

// llama_token, llama_tokenize, etc. are available via the transitive
// public import of llama_imports in llm.llama.model.

/**
 * Wraps a llama.cpp Model and provides tokenization for embedding.
 *
 * The class manages a pre-allocated token buffer for efficient
 * tokenization of short texts, and exposes the raw model, context,
 * and vocabulary pointers for downstream use.
 *
 * A reusable batch structure is pre-allocated to avoid re-allocating
 * token batches on every embed() call. Positions and sequence IDs
 * are pre-initialised once and reused, matching the behaviour of
 * llama_batch_get_one() from llama.cpp.
 */
class LlamaEmbedder : Embedder {
    private {
        string name;
        Model _model;
        llama_token[] _smallTokens;
        int _smallTokenSize;
        llama_token[] _batchTokens;
        llama_pos[] _batchPositions;
        /// Pre-allocated n_seq_id values (all 1, one per token).
        int32_t[] _nSeqIdValues;
        /// Pre-allocated sequence ID values (all 0, one per token).
        llama_seq_id[] _seqIdStorage;
        /// Pointer array: each entry points to the corresponding _seqIdStorage entry.
        llama_seq_id*[] _seqIdPtrs;
        llama_batch _batch;
        bool _destroyed = false;
    }

    /**
     * Construct a LlamaEmbedder wrapping the given Model.
     *
     * Params:
     *   model = a loaded Model instance with a valid vocabulary and context
     *   smallTokenSize = size of the pre-allocated small tokenization buffer.
     *     If <= 0, a heuristic is used: max(128, context_size / 8).
     *     Must not exceed the context n_batch capacity.
     *
     * Throws:
     *   Exception if model is null, or if the underlying context, vocabulary,
     *   or embedding dimension are invalid, or if smallTokenSize is invalid.
     */
    this(string name, Model model, int smallTokenSize = 0) {
        if (model is null)
            throw new Exception("LlamaEmbedder: model must not be null");
        if (model.model is null)
            throw new Exception("LlamaEmbedder: model pointer must not be null");
        if (model.ctx is null)
            throw new Exception("LlamaEmbedder: model context must not be null");
        if (model.vocab is null)
            throw new Exception("LlamaEmbedder: model vocabulary must not be null");
        if (llama_model_n_embd(model.model) <= 0)
            throw new Exception("LlamaEmbedder: model must have an embedding dimension > 0");

        this.name = name;
        this._model = model;

        if (smallTokenSize <= 0) {
            smallTokenSize = max(128, cast(int)(llama_n_ctx(model.ctx) / 8));
        }
        if (smallTokenSize <= 0)
            throw new Exception("LlamaEmbedder: smallTokenSize must be > 0");
        this._smallTokenSize = smallTokenSize;
        this._smallTokens = new llama_token[smallTokenSize];

        int nBatch = cast(int) llama_n_batch(model.ctx);
        if (nBatch <= 0) {
            nBatch = 512;
        }

        if (smallTokenSize > nBatch) {
            throw new Exception("LlamaEmbedder: smallTokenSize (" ~ to!string(
                    smallTokenSize) ~ ") exceeds batch capacity (" ~ to!string(nBatch) ~ ")");
        }

        this._batchTokens = new llama_token[nBatch];

        this._batchPositions = new llama_pos[nBatch];
        foreach (i; 0 .. nBatch)
            this._batchPositions[i] = cast(llama_pos) i;

        this._nSeqIdValues = new int32_t[nBatch];
        foreach (i; 0 .. nBatch)
            this._nSeqIdValues[i] = 1;

        this._seqIdStorage = new llama_seq_id[nBatch];
        this._seqIdPtrs = new llama_seq_id*[nBatch];
        foreach (i; 0 .. nBatch) {
            this._seqIdStorage[i] = 0;
            this._seqIdPtrs[i] = &this._seqIdStorage[i];
        }

        this._batch.n_tokens = 0;
        this._batch.token = this._batchTokens.ptr;
        this._batch.embd = null;
        this._batch.pos = this._batchPositions.ptr;
        this._batch.n_seq_id = this._nSeqIdValues.ptr;
        this._batch.seq_id = this._seqIdPtrs.ptr;
        this._batch.logits = null;
    }

    override string modelName() {
        return name;
    }

    /**
     * Return the embedding vector dimensions of this model.
     *
     * Queries the underlying llama.cpp model for its embedding
     * output dimensionality.
     *
     * Returns:
     *   The number of dimensions in the embedding vector, or 0 if
     *   destroy() has been called or the model has no embedding head.
     */
    override long dimensions() {
        if (_destroyed)
            return 0;
        return llama_model_n_embd(_model.model);
    }

    /**
     * Return the maximum batch size in approximate number of characters.
     *
     * Returns:
     *   Approximate batch, 0 if destroy() has been called.
     */
    override int batchSize() {
        import llm.common.config : ApproxTokenSize;

        if (_destroyed)
            return 0;
        return cast(int) llama_n_batch(_model.ctx) * ApproxTokenSize;
    }

    /**
     * Destroy the embedded model and mark this instance as destroyed.
     *
     * Idempotent — safe to call multiple times. All subsequent method
     * calls will return safe defaults (e.g. modelName() returns
     * "&lt;destroyed&gt;", dimensions() returns 0).
     */
    override void destroy() {
        if (_destroyed)
            return;
        _destroyed = true;
        _smallTokens = null;
        _batchTokens = null;
        _batchPositions = null;
        _nSeqIdValues = null;
        _seqIdStorage = null;
        _seqIdPtrs = null;
        _batch.token = null;
        _batch.pos = null;
        _batch.n_seq_id = null;
        _batch.seq_id = null;
        _batch.n_tokens = 0;
        _model.destroy;
    }

    /**
     * Tokenize text using the model's vocabulary.
     *
     * Uses a two-pass approach: first pass queries the required buffer
     * size, then tokenizes into a pre-allocated small buffer if the
     * result fits, or a newly allocated array otherwise. No special
     * tokens are added (add_special=false).
     *
     * The returned slice is a copy if the internal small buffer was
     * used, guaranteeing that repeated calls do not silently corrupt
     * previously returned slices.
     *
     * Params:
     *   text = UTF-8 string to tokenize
     *
     * Returns:
     *   An array of token IDs, trimmed to the actual token count.
     *   Never returns null — may return an empty array for empty input.
     *
     * Throws:
     *   Exception if llama_tokenize fails (e.g. invalid UTF-8).
     */
    private llama_token[] tokenize(string text) @trusted
    in (text !is null) {
        auto vocab = _model.vocab;

        // query required buffer size
        int needed = llama_tokenize(vocab, text.ptr, to!int(text.length), null, 0, false, // add_special = false
                true); // parse_special = true
        if (needed < 0)
            needed = -needed;

        llama_token[] buf = (needed <= _smallTokens.length) ? _smallTokens : new llama_token[needed];

        // actual tokenization into the buffer
        int actual = llama_tokenize(vocab, text.ptr, to!int(text.length),
                buf.ptr, to!int(buf.length), false, // add_special = false
                true); // parse_special = true
        if (actual < 0)
            throw new Exception(
                    "LlamaEmbedder.tokenize: llama_tokenize failed (code: " ~ to!string(
                    actual) ~ ")");

        return (buf is _smallTokens) ? buf[0 .. actual].dup : buf[0 .. actual];
    }

    /**
     * Convert token IDs back to text using llama_token_to_piece.
     *
     * Iterates over the provided token array, decoding each token
     * individually via the model vocabulary and accumulating the
     * resulting character pieces into a single string.
     *
     * Params:
     *   tokens = array of token IDs to decode
     *
     * Returns:
     *   The reconstructed text string, or empty string if the
     *   embedder has been destroyed.
     */
    private string detokenize(llama_token[] tokens) {
        if (_destroyed)
            return "";
        auto result = appender!string;
        char[256] buf = '\0';
        foreach (token; tokens) {
            int len = llama_token_to_piece(_model.vocab, token, buf.ptr, buf.sizeof, 0, true);
            if (len > 0) {
                result.put(buf[0 .. len]);
            } else if (len < 0) {
                // Buffer too small; retry with required size
                int needed = -len;
                char[] bigBuf = new char[needed];
                int actual = llama_token_to_piece(_model.vocab, token,
                        bigBuf.ptr, needed, 0, true);
                if (actual > 0)
                    result.put(bigBuf[0 .. actual]);
            }
        }
        return result.data;
    }

    /**
     * Produce an embedding vector for the given text via llama.cpp.
     *
     * Properly handles all error conditions with early returns:
     * - Destroyed embedder returns error string (not segfault)
     * - Tokenization failure returns error string
     * - Encode failure returns error string (not silent fallthrough)
     * - Null embedding pointer returns error string (not segfault)
     *
     * Uses the pre-allocated reusable batch to avoid per-call heap
     * allocation of batch structures. Positions and sequence IDs are
     * pre-initialised and reused.
     *
     * Params:
     *   text = UTF-8 string to embed
     *
     * Returns:
     *   EmbedResult containing either a float[] embedding vector on success,
     *   or a string error message on failure.
     */
    override EmbedResult embed(string text) {
        if (_destroyed)
            return EmbedResult(EmbedError("Embedder has been destroyed"));

        llama_token[] tokens;
        try {
            tokens = tokenize(text);
        } catch (Exception e) {
            return EmbedResult(EmbedError("Tokenization failed: " ~ e.msg));
        }

        // Guard against empty input — zero-token batch produces undefined
        // behaviour in llama_encode (may succeed silently, return garbage,
        // or crash).
        if (tokens.length == 0) {
            return EmbedResult(EmbedError("Empty input produced zero tokens"));
        }

        if (tokens.length > _batchTokens.length) {
            return EmbedResult(EmbedError(
                    "Token count exceeds batch capacity (" ~ to!string(_batchTokens.length) ~ ")"));
        }
        _batchTokens[0 .. tokens.length] = tokens[];
        _batch.n_tokens = cast(int) tokens.length;
        // Positions, n_seq_id, and seq_id are pre-initialised in the
        // constructor and remain valid for any token count ≤ nBatch.

        // Encode - must return early on failure.
        // encode() returns false either when model.ctx is null (destroyed
        // model or null pointer) or when llama_encode returns a non-zero
        // error code. Either way, the embedding is unavailable.
        if (!encode(_model, _batch)) {
            return EmbedResult(EmbedError("Failed to encode tokens for embedding"));
        }

        auto n = llama_model_n_embd(_model.model);
        auto e = llama_get_embeddings_seq(_model.ctx, 0);
        if (e is null) {
            return EmbedResult(EmbedError("Embeddings returned null"));
        }

        return EmbedResult(e[0 .. n].dup);
    }
}
