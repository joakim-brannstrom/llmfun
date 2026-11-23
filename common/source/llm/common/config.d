module llm.common.config;

import logger = std.logger;
import std.conv : to, text;
import std.sumtype : SumType, match;

import my.path : Path;

/// Approximate number of characters per token. Used for estimating token counts from string lengths.
immutable ApproxTokenSize = 2;

/// Approximate token cost of one image. A vision encoder emits a bounded
/// number of tokens per image, a few hundred to ~1500 depending on resolution.
immutable ApproxImageTokens = 1500;

enum EndpointType {
    unknown,
    openAiv1,
    llamaCpp,
    deepseek
}

struct ServerConfig {
    string url;
    string promptUrl = "v1/completion";
    string chatUrl = "v1/chat/completions";
    string slotUrl = "slots";
    string embedUrl = "v1/embeddings";
    long timeoutSeconds;
    long httpVerbosity;
    bool verifySslCert = true;
    long maxRetries = 3; // maximum number of retries for transient failures
    long backoffMs = 500; // initial backoff in milliseconds (exponential)
    string jsonFields; // a JSON object that is merged into the chat message

    /** Environment variable to read the API key from.
     * API key for Bearer token authentication (e.g. OpenAI API key).
     * If empty or the environment variable is not set a warning is raised.
     * Leave empty for servers that do not require authentication (e.g. local
     * llama.cpp), but remember to also set warnIfNoApiKey to false.
     */
    string apiKeyEnv;

    /// If true, emit a warning when no API key is configured for a model server. Defaults to true.
    bool warnIfNoApiKey = true;

    // Type of end point
    string type;

    EndpointType toType() {
        try {
            return type.to!EndpointType;
        } catch (Exception e) {
            logger.warningf("Unknown type '%s' for server url '%s'", type, url);
        }
        return EndpointType.unknown;
    }

    string toChatUrl() {
        return i"$(url)/$(chatUrl)".text;
    }

    string toPromptUrl() {
        return i"$(url)/$(promptUrl)".text;
    }

    string toSlotUrl() {
        return i"$(url)/$(slotUrl)".text;
    }

    string toEmbedUrl() {
        return i"$(url)/$(embedUrl)".text;
    }
}

/// Execution mode for a local embedding model.
enum EmbedMode {
    /// No GPU involvement: weights and computation stay on the CPU.
    cpu,
    /// Weights stay in system memory; llama.cpp may offload individual
    /// operations to a GPU.
    mixed,
    /// Offload the model (layers) to the GPU.
    gpu,
}

/// Configuration for a local embedding backend (llama.cpp).
struct LocalEmbedConfig {
    string modelName;
    long chunkSize = 512;
    long dimensions;
    /// How the local model runs: cpu, mixed or gpu.
    EmbedMode mode = EmbedMode.cpu;
    Path modelPath;
    long cpuThreads; // 0: use as many as there are cores
    long cpuThreadsBatch; // 0: use as many as there are cores
    long uBatch = 512;
    long nBatch = 512;

    // asymmetric models prefix
    string documentPrefix;
    string queryPrefix;
}

/// Configuration for a remote embedding backend (HTTP API).
struct RemoteEmbedConfig {
    ServerConfig server;
    string modelName;
    long chunkSize = 512;
    long dimensions;

    // asymmetric models prefix
    string documentPrefix;
    string queryPrefix;
}

/// Union type for embedding backend configuration.
alias EmbedConfig = SumType!(RemoteEmbedConfig, LocalEmbedConfig);
