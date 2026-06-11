// HTTP-based embedder using the requests library.

module llm.rag.embedder_http;

import logger = std.logger;
import std.algorithm : map, joiner;
import std.array : appender, empty;
import std.format : format;
import std.json : JSONValue, parseJSON, JSONType;
import std.string : split;
import std.sumtype : SumType, match;
import core.time : dur;

import requests;

import llm.rag.embedder;
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
        Request rq;
        LibRequestConfig rqCfg;
    }

    this(RemoteEmbedConfig cfg) {
        import llm.config : getEnvApiKey;

        this.cfg = cfg;

        auto apiKey = cfg.server.apiKey.empty ? getEnvApiKey() : cfg.server.apiKey;
        auto headers = ["Content-Type": "application/json"];
        if (!apiKey.empty)
            headers["Authorization"] = "Bearer " ~ apiKey;
        this.rqCfg = LibRequestConfig(headers: headers, maxRetries: cfg.server.maxRetries,
                timeout: cfg.server.timeoutSeconds.dur!"seconds", sslSetVerifyPeer: cfg.server.verifySslCert,
                backoffBaseMs: cfg.server.backoffMs, verbosity: cfg.server.httpVerbosity,
                keepAlive: cfg.server.keepAlive);
    }

    override void destroy() {
    }

    override EmbedResult embed(string text) {
        import llm.utility : getValue;

        bool hasError = true;

        EmbedResult parseHttp(HttpResult r) {
            logger.tracef(r.statusCode != 200, "RemoteEmbedder: Response status %d", r.statusCode);

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
        EmbedResult rval;

        for (int i = 0; i < MaxRetryEmbedder && hasError; ++i) {
            hasError = false;
            auto result = httpPostWithRetry(rq, cfg.server.toEmbedUrl, jsonReq.toString, rqCfg);
            result.match!((HttpResult r) { rval = parseHttp(r); }, (HttpError e) {
                logger.errorf("RemoteEmbedder: HTTP error %s: %s", e.statusCode, e.errorMsg);
                rval = EmbedResult(e.errorMsg);
            });
        }
        return rval;
    }

    override int batchSize() {
        return cast(int) cfg.nBatch;
    }
}
