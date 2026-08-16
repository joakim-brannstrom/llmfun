// HTTP-based embedder using the curl.

module llm.rag.embedder_http;

import logger = std.logger;
import std.algorithm : map, joiner;
import std.array : appender, empty;
import std.format : format;
import std.json : JSONValue, parseJSON, JSONType, JSONOptions;
import std.string : split;
import std.sumtype : SumType, match;
import core.time : dur;

import llm.common.embedder;
import llm.query;
import llm.config : RemoteEmbedConfig;

// This should not lead to 3x the number of retry because of the builtin retry
// in httpPostWithRetry. This try to catch another type of error which is an OK
// reply from the embedder but it returned an empty embedder vector.
private immutable MaxRetryEmbedder = 3;

/// HTTP-based embedding backend for OpenAI-compatible API endpoints.
class RemoteEmbedder : Embedder {
    private {
        RemoteEmbedConfig cfg;
        LibRequestConfig rqCfg;
    }

    this(RemoteEmbedConfig cfg) {
        import llm.config : getEnvApiKey;

        this.cfg = cfg;

        auto apiKey = cfg.server.apiKeyEnv.empty ? "" : getEnvApiKey(cfg.server.apiKeyEnv);
        // No "Content-Type" here: setPostData adds it for POST requests.
        auto headers = string[string].init;
        if (!apiKey.empty)
            headers["Authorization"] = "Bearer " ~ apiKey;
        this.rqCfg = LibRequestConfig(headers: headers, maxRetries: cfg.server.maxRetries,
                timeout: cfg.server.timeoutSeconds.dur!"seconds", sslSetVerifyPeer: cfg.server.verifySslCert,
                backoffBaseMs: cfg.server.backoffMs, verbosity: cfg.server.httpVerbosity);
    }

    override void destroy() {
    }

    override string modelName() {
        return cfg.name;
    }

    override long dimensions() {
        return cfg.dimensions;
    }

    override EmbedResult embed(string text) {
        import llm.utility : getValue;

        bool hasError = true;

        EmbedResult parseHttp(HttpResult r) {
            logger.tracef(r.statusCode != 200, "RemoteEmbedder: Response status %s", r.statusCode);

            JSONValue json;
            try {
                json = parseJSON(r.body);
            } catch (Exception e) {
                logger.trace(r.body);
                logger.trace(e.msg);
            }

            // some implementations of the REST endpoint return an object where
            // the embedding is in the data field.
            json = getValue(json, (v) => json["data"], json);
            auto embeddings = getValue(json, (v) => v.array[0]["embedding"].array, JSONValue[].init);
            if (!embeddings.empty && embeddings[0].type == JSONType.array) {
                embeddings = embeddings[0].array;
            }

            float[] resultVec;
            foreach (e; embeddings) {
                try {
                    resultVec ~= cast(float) e.floating;
                } catch (Exception e) {
                }
            }
            if (resultVec.empty) {
                logger.trace(json);
                logger.trace(embeddings);
                hasError = true;
            } else {
                logger.tracef("RemoteEmbedder: Embedding dimensions: %s", resultVec.length);
            }
            return EmbedResult(resultVec);
        }

        JSONValue jsonReq;
        jsonReq["model"] = cfg.name;
        jsonReq["input"] = text;
        jsonReq["encoding_format"] = "float";
        EmbedResult rval;
        for (int i = 0; i < MaxRetryEmbedder && hasError; ++i) {
            hasError = false;
            auto result = httpPostWithRetry(cfg.server.toEmbedUrl,
                    jsonReq.toString(JSONOptions.doNotEscapeSlashes), rqCfg);
            result.match!((HttpResult r) { rval = parseHttp(r); }, (HttpError e) {
                hasError = true;
                rval = EmbedResult(EmbedError(e.errorMsg));
            });
        }

        return rval;
    }

    override int batchSize() {
        import llm.common.config : ApproxTokenSize;

        return cast(int) cfg.nBatch * ApproxTokenSize;
    }

    override bool supportsTokenization() @safe {
        return false;
    }

    override int[] tokenize(string txt) @safe {
        return null;
    }

    override string detokenize(int[] tokens) @safe {
        return null;
    }

    override EmbedResult embed(int[] tokens) @safe {
        return EmbedResult(EmbedError("Embedder do not support tokens"));
    }
}
