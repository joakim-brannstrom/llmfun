/**
 * Model class and LlamaParams struct for wrapping llama.cpp C API.
 *
 * This module provides a class-based wrapper around the llama.cpp model,
 * context, and vocabulary. Unlike the GPL-licensed dllm reference which
 * uses a struct-based approach, this implementation uses a class for
 * managed resource lifecycle with deterministic destruction.
 *
 * License: MPL-2.0
 */
module llm.llama.model;

import std.string : toStringz;
public import llama_imports;

/**
 * Wraps a llama.cpp model, context, and vocabulary.
 *
 * Manages the lifecycle of three C resources:
 *   - `llama_model*`   — loaded from a file path
 *   - `llama_context*`  — created from the model
 *   - `llama_vocab*`    — retrieved from the model (owned by the model)
 *
 * Call `destroy()` explicitly before program exit to guarantee proper
 * cleanup. The destructor provides a limited safety net but cannot
 * safely call C functions after the C shared library has been unloaded
 * during GC finalization at shutdown.
 */
class Model {
    private {
        llama_model* _model;
        llama_context* _ctx;
        llama_vocab* _vocab;

        Model cloneOf;
        LlamaParams params;
    }

    // Make a clone of the model which reuse the model but have its own ctx and vocab.
    this(Model model) {
        cloneOf = model;
        params = model.params;
        _model = model.model;

        _ctx = llama_init_from_model(_model, params.ctxParams);
        if (_ctx is null) {
            throw new Exception("Failed to create context from model");
        }

        _vocab = llama_model_get_vocab(_model);
        if (_vocab is null) {
            llama_free(_ctx);
            _ctx = null;
            throw new Exception("Failed to retrieve vocabulary from model");
        }
    }

    /**
     * Load a model from the given path with the specified parameters.
     *
     * Throws: Exception if model loading, context creation, or vocab
     *         retrieval fails. All successfully allocated resources are
     *         cleaned up before throwing.
     */
    this(string modelPath, LlamaParams params) {
        this.params = params;
        _model = llama_model_load_from_file(modelPath.toStringz, params.modelParams);
        if (_model is null) {
            throw new Exception("Failed to load model: " ~ modelPath);
        }

        _ctx = llama_init_from_model(_model, params.ctxParams);
        if (_ctx is null) {
            llama_model_free(_model);
            _model = null;
            throw new Exception("Failed to create context from model: " ~ modelPath);
        }

        _vocab = llama_model_get_vocab(_model);
        if (_vocab is null) {
            llama_free(_ctx);
            _ctx = null;
            llama_model_free(_model);
            _model = null;
            throw new Exception("Failed to retrieve vocabulary from model: " ~ modelPath);
        }
    }

    /**
     * Free all resources.
     *
     * Idempotent — safe to call multiple times. Sets all pointers to null
     * after freeing. The vocab pointer is owned by the model and is freed
     * implicitly when the model is freed.
     */
    void destroy() {
        if (_ctx !is null) {
            llama_free(_ctx);
            _ctx = null;
        }
        if (_model !is null && cloneOf is null) {
            llama_model_free(_model);
            _model = null;
        }
        _vocab = null;
    }

    @safe nothrow llama_model* model() {
        return _model;
    }

    @safe nothrow llama_context* ctx() {
        return _ctx;
    }

    @safe nothrow llama_vocab* vocab() {
        return _vocab;
    }
}

/**
 * Default parameters for creating a Model.
 *
 * Use `LlamaParams.make()` to obtain default model and context parameters,
 * then customise them before passing to the `Model` constructor.
 *
 * The `modelParams` and `ctxParams` fields are package-visible within
 * `llm.llama` so that helper functions like `contextEmbedding` and the
 * `Model` constructor can access them directly.
 */
struct LlamaParams {
    llama_model_params modelParams;
    llama_context_params ctxParams;

    static LlamaParams make() {
        LlamaParams p;
        p.modelParams = llama_model_default_params();
        p.ctxParams = llama_context_default_params();
        return p;
    }

    ref llama_context_params ctx() {
        return ctxParams;
    }
}

/**
 * Configure `LlamaParams` for embedding use.
 *
 * Sets the context parameters for embedding extraction:
 *   - `embeddings`    = true
 *   - `pooling_type`  = `LLAMA_POOLING_TYPE_CLS`
 *   - `n_batch`       = the specified batch size
 *
 * Returns the modified `LlamaParams` so calls can be chained.
 */
LlamaParams contextEmbedding(LlamaParams params, uint nBatch) {
    import std.parallelism : totalCPUs;

    params.ctxParams.n_threads = totalCPUs;
    params.ctxParams.n_threads_batch = totalCPUs;

    params.ctxParams.no_perf = true;
    params.ctxParams.embeddings = true;
    params.ctxParams.op_offload = true;
    params.ctxParams.offload_kqv = true;
    params.ctxParams.pooling_type = LLAMA_POOLING_TYPE_CLS;
    params.ctxParams.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_AUTO;

    params.ctxParams.n_ctx = nBatch;
    params.ctxParams.n_batch = nBatch;
    params.ctxParams.n_ubatch = nBatch;

    return params;
}

LlamaParams onlyCpu(LlamaParams p) {
    p.modelParams.n_gpu_layers = 0;
    p.ctxParams.offload_kqv = false;
    return p;
}

LlamaParams onlyGpu(LlamaParams p) {
    p.modelParams.n_gpu_layers = -1;
    p.ctxParams.offload_kqv = true;
    return p;
}

// ---------------------------------------------------------------------------
// Unit tests
// ---------------------------------------------------------------------------

/**
 * Verify that LlamaParams.make() returns defaults consistent with
 * the llama.cpp API, and that contextEmbedding modifies them correctly.
 *
 * Note: Full integration tests that load a real model are in the
 * separate integration test suite.
 */
unittest {
    // --- LlamaParams.make() defaults ---
    auto p = LlamaParams.make();

    // Model params: n_gpu_layers defaults to 0 (CPU only)
    assert(p.modelParams.n_gpu_layers == 0, "Default n_gpu_layers should be 0");

    // Context params: n_ctx defaults to 0 (use model's value)
    assert(p.ctxParams.n_ctx == 0, "Default n_ctx should be 0 (from model)");

    // Context params: embeddings defaults to false
    assert(p.ctxParams.embeddings == false, "Default embeddings should be false");

    // --- contextEmbedding() ---
    auto ep = contextEmbedding(p, 512);

    assert(ep.ctxParams.embeddings == true, "embeddings should be true after contextEmbedding");

    assert(ep.ctxParams.pooling_type == LLAMA_POOLING_TYPE_CLS,
            "pooling_type should be LLAMA_POOLING_TYPE_CLS after contextEmbedding");

    assert(ep.ctxParams.n_batch == 512, "n_batch should be 512 after contextEmbedding");
}
