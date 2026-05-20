// HTTP-based embedder using the requests library.

module llm.rag.embedder_http;

import logger = std.logger;
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

    /// Create a RemoteEmbedder with the given configuration.
    this(RemoteEmbedConfig cfg) {
        this.cfg = cfg;
    }

    override void destroy() {
    }

    /// Produce an embedding vector via HTTP POST to /embeddings endpoint.
    override EmbedResult embed(string text) {
        auto body = format!"{\"model\": \"%s\", \"input\": \"%s\"}"(cfg.name, escapeJson(text));

        auto headers = ["Content-Type": "application/json"];
        if (!cfg.server.apiKey.empty) {
            headers["Authorization"] = format!"Bearer %s"(cfg.server.apiKey);
        }

        auto result = httpPostWithRetry(Request(), cfg.server.embedUrl, body, headers,
                cfg.server.maxRetries, cfg.server.timeoutSeconds, cfg.server.backoffMs);

        return result.match!((HttpPostResult r) {
            logger.tracef("RemoteEmbedder: Response status %d", r.statusCode);

            auto json = parseJSON(r.body);
            auto embedding = json["data"][0]["embedding"].array;
            float[] resultVec;
            foreach (e; embedding) {
                resultVec ~= cast(float) e.floating;
            }
            logger.infof("RemoteEmbedder: Embedding dimensions: %d", resultVec.length);
            return EmbedResult(resultVec);
        }, (HttpPostError e) {
            logger.errorf("RemoteEmbedder: HTTP error %d: %s", e.statusCode, e.errorMsg);
            return EmbedResult(e.errorMsg);
        });
    }

    /// Simple text-based tokenization (split on whitespace).
    override int[] tokenize(string text) {
        import std.digest.murmurhash : digest, MurmurHash3;

        auto words = split(text);
        int[] tokens;
        foreach (w; words) {
            if (!w.empty) {
                auto h = digest!(MurmurHash3!32)(w);
                tokens ~= cast(int) h[0];
            }
        }
        return tokens;
    }

    /// Simple detokenization (join with spaces).
    override string detokenize(int[] tokens) {
        auto data = appender!string();
        foreach (t; tokens) {
            if (!data.empty)
                data.put(" ");
            data.put(format!"%s"(t));
        }
        return data[];
    }

    /// Return a default batch size for remote requests.
    override int batchSize() {
        return 8192;
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
