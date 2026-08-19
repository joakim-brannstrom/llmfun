module llm.local;

import core.sync : Mutex;
import std.algorithm : map;
import std.sumtype : match;
import logger = std.logger;

import llm.common.config : EmbedConfig, LocalEmbedConfig, RemoteEmbedConfig;
import llm.common.embedder : Embedder, registerEmbedderFactory;
import llm.llama.model : Model;

private:

shared Mutex createEmbedder;

shared Mutex modelIndex;
shared Model[string] models;

Model getModel(string name) nothrow {
    modelIndex.lock_nothrow();
    scope (exit)
        modelIndex.unlock_nothrow();

    if (auto m = name in models) {
        return cast()*m;
    }
    return null;
}

void addModel(string name, Model model) nothrow {
    modelIndex.lock_nothrow();
    scope (exit)
        modelIndex.unlock_nothrow();

    models[name] = cast(shared) model;
}

extern (C) void initLlmfunLocalModel() {
    llamaInit();
    registerEmbedderFactory("local", &createLocalEmbedder);
}

extern (C) void deinitLlmfunLocalModel() {
    llamaDeinit();
}

Embedder createLocalEmbedder(EmbedConfig config) {
    return config.match!((RemoteEmbedConfig _) => null, // not our type
            (LocalEmbedConfig local) {
        import llm.llama.model : Model, LlamaParams, contextEmbedding, onlyCpu, onlyGpu;
        import llm.local.llama_embedder : LlamaEmbedder;

        createEmbedder.lock_nothrow();
        scope (exit)
            createEmbedder.unlock_nothrow();

        if (auto m = getModel(local.modelName)) {
            return new LlamaEmbedder(local.modelName, new Model(m), destroyModel: true);
        }

        auto params = contextEmbedding(LlamaParams.make(), cast(uint) local.nBatch);
        if (local.onlyCpu) {
            params = params.onlyCpu;
        } else {
            params = params.onlyGpu;
        }

        auto model = new Model(local.modelPath, params);
        addModel(local.modelName, model);

        return new LlamaEmbedder(local.modelName, model, destroyModel: false);
    });
}

void llamaInit() nothrow {
    import llm.llama.llama_import : llama_backend_init, llama_log_set, ggml_log_level;

    createEmbedder = cast(shared) new Mutex;
    modelIndex = cast(shared) new Mutex;

    static struct LlamaLog {
        string buf;
    }

    static LlamaLog userData;

    extern (C) static void llamaLog(ggml_log_level level, const char* rawStr, void* userData) {
        import std.array : empty;
        import std.string : fromStringz, strip;

        auto data = cast(LlamaLog*) userData;
        auto str = fromStringz(rawStr);

        if (!data.buf.empty) {
            str = data.buf ~ str;
            data.buf = null;
        }
        str = str.strip;

        final switch (level) {
        case ggml_log_level.GGML_LOG_LEVEL_NONE:
            logger.tracef("[%s]: %s", level, str);
            break;
        case ggml_log_level.GGML_LOG_LEVEL_DEBUG:
            logger.tracef("[%s]: %s", level, str);
            break;
        case ggml_log_level.GGML_LOG_LEVEL_INFO:
            logger.tracef("[%s]: %s", level, str);
            break;
        case ggml_log_level.GGML_LOG_LEVEL_WARN:
            logger.tracef("[%s]: %s", level, str);
            break;
        case ggml_log_level.GGML_LOG_LEVEL_ERROR:
            logger.error(str);
            break;
        case ggml_log_level.GGML_LOG_LEVEL_CONT:
            data.buf ~= str;
            break;
        }
    }

    try {
        llama_backend_init();
        llama_log_set(&llamaLog, &userData);
    } catch (Exception e) {
        try {
            logger.warningf("Unable to initialize llama backend: %s", e.msg);
        } catch (Exception e) {
        }
    }
}

void llamaDeinit() nothrow {
    import llm.llama.llama_import : llama_backend_free;

    modelIndex.lock_nothrow();
    scope (exit)
        modelIndex.unlock_nothrow();

    try {
        foreach (m; (cast() models).byValue.map!(a => cast() a)) {
            try {
                m.destroy;
            } catch (Exception e) {
            }
        }
        llama_backend_free();
    } catch (Exception e) {
        try {
            logger.warningf("Unable to deinit llama backedn: %s", e.msg);
        } catch (Exception e) {
        }
    }
    models = null;
}
