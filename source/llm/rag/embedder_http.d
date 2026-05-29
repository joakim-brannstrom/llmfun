// HTTP-based embedder using the requests library.

module llm.rag.embedder_http;

import logger = std.logger;
import std.algorithm : map, joiner;
import std.array : appender, empty;
import std.format : format;
import std.json : JSONValue, parseJSON, JSONType;
import std.string : split;
import std.sumtype : SumType, match;

import requests;

import llm.rag.embedder;
import llm.query;
import llm.config : RemoteEmbedConfig;

/// HTTP-based embedding backend for OpenAI-compatible API endpoints.
class RemoteEmbedder : Embedder {
    private {
        RemoteEmbedConfig cfg;
        string apiKey;
    }

    this(RemoteEmbedConfig cfg) {
        import llm.config : getEnvApiKey;

        this.cfg = cfg;
        this.apiKey = cfg.server.apiKey.empty ? getEnvApiKey() : cfg.server.apiKey;
    }

    override void destroy() {
    }

    override EmbedResult embed(string text) {
        import llm.utility : getValue;

        auto body = format!"{\"model\": \"%s\", \"input\": \"%s\"}"(cfg.name, escapeJson(text));

        auto headers = ["Content-Type": "application/json"];
        if (!apiKey.empty) {
            headers["Authorization"] = format!"Bearer %s"(apiKey);
        }

        auto result = httpPostWithRetry(Request(), cfg.server.toEmbedUrl, body, headers,
                cfg.server.maxRetries, cfg.server.timeoutSeconds, cfg.server.backoffMs);

        return result.match!((HttpPostResult r) {
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
            } else {
                logger.tracef("RemoteEmbedder: Embedding dimensions: %s", resultVec.length);
            }
            return EmbedResult(resultVec);
        }, (HttpPostError e) {
            logger.errorf("RemoteEmbedder: HTTP error %s: %s", e.statusCode, e.errorMsg);
            return EmbedResult(e.errorMsg);
        });
    }

    override int batchSize() {
        return cast(int) cfg.nBatch;
    }

    private string escapeJson(string s) {
        auto data = appender!string();
        foreach (c; s) {
            switch (c) {
            case '"':
                data.put("\\\"");
                break;
            case '\\':
                data.put("\\\\");
                break;
            case '\n':
                data.put("\\n");
                break;
            case '\r':
                data.put("\\r");
                break;
            case '\t':
                data.put("\\t");
                break;
            default:
                data.put(c);
                break;
            }
        }
        return data[];
    }
}
