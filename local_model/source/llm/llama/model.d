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

import llm.common.config : EmbedMode;

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
 *   - `n_batch`       = the specified batch size
 *
 * Execution-device parameters (weights placement, offloading) are set
 * separately by `applyMode()`.
 *
 * Returns the modified `LlamaParams` so calls can be chained.
 */
LlamaParams contextEmbedding(LlamaParams params, uint ctxSize, uint nBatch,
        uint uBatch, int threads, int threadsBatch) {
    import std.parallelism : totalCPUs;

    params.ctxParams.n_threads = threads <= 0 ? totalCPUs : threads;
    params.ctxParams.n_threads_batch = threadsBatch <= 0 ? totalCPUs : threadsBatch;

    params.ctxParams.no_perf = true;
    params.ctxParams.embeddings = true;
    // llama.cpp will use the models default.
    params.ctxParams.pooling_type = LLAMA_POOLING_TYPE_UNSPECIFIED;
    params.ctxParams.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_AUTO;

    params.ctxParams.n_ctx = ctxSize;
    params.ctxParams.n_batch = nBatch;
    params.ctxParams.n_ubatch = uBatch;

    return params;
}

/// Empty, NULL-terminated device list: tells llama.cpp that no device exists
/// for offloading. Static because the loaded model keeps the pointer.
private __gshared ggml_backend_dev_t[1] noDevices = [null];

/**
 * Apply `mode` to `LlamaParams` and return it.
 *
 * `cpu` keeps everything on the CPU: weights stay in system memory and no
 * device is offered to llama.cpp, so neither operations nor buffers can end
 * up on a GPU. `mixed` keeps the weights in system memory and lets llama.cpp
 * offload individual operations to a GPU. `gpu` offloads all model layers
 * to the GPU.
 */
LlamaParams applyMode(LlamaParams p, EmbedMode mode) {
    final switch (mode) {
    case EmbedMode.cpu:
        p.modelParams.n_gpu_layers = 0;
        p.modelParams.devices = noDevices.ptr;
        p.ctxParams.offload_kqv = false;
        p.ctxParams.op_offload = false;
        break;
    case EmbedMode.mixed:
        p.modelParams.n_gpu_layers = 0;
        p.ctxParams.offload_kqv = false;
        p.ctxParams.op_offload = true;
        break;
    case EmbedMode.gpu:
        p.modelParams.n_gpu_layers = -1;
        p.ctxParams.offload_kqv = true;
        p.ctxParams.op_offload = true;
        break;
    }
    return p;
}

/**
 * Verify the LlamaParams helpers: applyMode maps each EmbedMode to the
 * expected llama.cpp knobs and contextEmbedding sizes the context.
 *
 * Note: Full integration tests that load a real model are in the
 * separate integration test suite.
 */
unittest {
    import std.parallelism : totalCPUs;

    // --- applyMode() ---
    {
        auto p = LlamaParams.make().applyMode(EmbedMode.cpu);
        assert(p.modelParams.n_gpu_layers == 0, "cpu mode must keep weights on the CPU");
        assert(p.modelParams.devices !is null, "cpu mode must pass an explicit device list");
        assert(p.modelParams.devices[0] is null, "cpu mode must not offer any device");
        assert(p.ctxParams.offload_kqv == false, "cpu mode must not offload KQV");
        assert(p.ctxParams.op_offload == false, "cpu mode must not offload operations");
    }
    {
        auto p = LlamaParams.make().applyMode(EmbedMode.mixed);
        assert(p.modelParams.n_gpu_layers == 0, "mixed mode must keep weights on the CPU");
        assert(p.ctxParams.offload_kqv == false, "mixed mode must not offload KQV");
        assert(p.ctxParams.op_offload == true, "mixed mode must allow op offloading");
    }
    {
        auto p = LlamaParams.make().applyMode(EmbedMode.gpu);
        assert(p.modelParams.n_gpu_layers == -1, "gpu mode must offload all layers");
        assert(p.ctxParams.offload_kqv == true, "gpu mode must offload KQV");
        assert(p.ctxParams.op_offload == true, "gpu mode must allow op offloading");
    }

    // --- contextEmbedding() ---
    auto ep = contextEmbedding(LlamaParams.make(), 512, 512, 512, 0, 0);

    assert(ep.ctxParams.embeddings == true, "embeddings should be true after contextEmbedding");
    assert(ep.ctxParams.n_ctx == 512, "n_ctx should be the requested context size");
    assert(ep.ctxParams.n_batch == 512, "n_batch should be the requested batch size");
    assert(ep.ctxParams.n_ubatch == 512, "n_ubatch should be the requested batch size");
    assert(ep.ctxParams.n_threads == cast(int) totalCPUs, "threads = 0 uses all CPUs");
    assert(ep.ctxParams.n_threads_batch == cast(int) totalCPUs, "threadsBatch = 0 uses all CPUs");

    auto ep2 = contextEmbedding(LlamaParams.make(), 512, 512, 512, 3, 4);
    assert(ep2.ctxParams.n_threads == 3, "explicit thread count must be used");
    assert(ep2.ctxParams.n_threads_batch == 4, "explicit batch thread count must be used");
}
