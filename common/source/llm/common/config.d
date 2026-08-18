module llm.common.config;

import logger = std.logger;
import std.conv : to;
import std.format : format;
import std.sumtype : SumType, match;

import my.path : Path;

/// Approximate number of characters per token. Used for estimating token counts from string lengths.
immutable ApproxTokenSize = 2;

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
        return format!"%s/%s"(url, chatUrl);
    }

    string toPromptUrl() {
        return format!"%s/%s"(url, promptUrl);
    }

    string toSlotUrl() {
        return format!"%s/%s"(url, slotUrl);
    }

    string toEmbedUrl() {
        return format!"%s/%s"(url, embedUrl);
    }
}

/// Configuration for a local embedding backend (llama.cpp).
struct LocalEmbedConfig {
    string name;
    long nBatch = 512;
    long dimensions;
    // if the model should run only on CPU
    bool onlyCpu = true;
    Path modelPath;
}

/// Configuration for a remote embedding backend (HTTP API).
struct RemoteEmbedConfig {
    ServerConfig server;
    string name;
    long nBatch = 512;
    long dimensions;
}

/// Union type for embedding backend configuration.
alias EmbedConfig = SumType!(RemoteEmbedConfig, LocalEmbedConfig);
