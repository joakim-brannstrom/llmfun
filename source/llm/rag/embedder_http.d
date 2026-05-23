// HTTP-based embedder using the requests library.

module llm.rag.embedder_http;

import logger = std.logger;
import std.algorithm : map, joiner;
import std.array : appender, empty;
import std.format : format;
import std.json : JSONValue, parseJSON;
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
    }

    this(RemoteEmbedConfig cfg) {
        logger.trace(cfg);
        this.cfg = cfg;
    }

    override void destroy() {
    }

    override EmbedResult embed(string text) {
        import llm.utility : getValue;

        auto body = format!"{\"model\": \"%s\", \"input\": \"%s\"}"(cfg.name, escapeJson(text));

        auto headers = ["Content-Type": "application/json"];
        if (!cfg.server.apiKey.empty) {
            headers["Authorization"] = format!"Bearer %s"(cfg.server.apiKey);
        }

        auto result = httpPostWithRetry(Request(), cfg.server.toEmbedUrl, body, headers,
                cfg.server.maxRetries, cfg.server.timeoutSeconds, cfg.server.backoffMs);

        return result.match!((HttpPostResult r) {
            logger.tracef("RemoteEmbedder: Response status %d", r.statusCode);

            auto json = parseJSON(r.body);
            // logger.trace(json);
            auto embeddings = getValue(json, (v) => v.array[0]["embedding"].array, JSONValue[].init);
            float[] resultVec;
            foreach (e; embeddings.map!((a => getValue(a, (v) => v.array, null))).joiner) {
                try {
                    resultVec ~= cast(float) e.floating;
                } catch (Exception e) {
                }
            }
            logger.tracef("RemoteEmbedder: Embedding dimensions: %s", resultVec.length);
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
