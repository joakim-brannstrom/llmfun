module llm.query;

import core.thread : Thread;
import core.time : dur;
import logger = std.logger;
import std.array : empty;
import std.conv : to, text;
import std.exception : collectException;
import std.json : JSONValue, parseJSON;
import std.stdio : writeln;
import std.sumtype : SumType, match;
import std.typecons : Nullable;
import std.utf : byUTF;

import requests;

import llm.chat;

struct RequestConfig {
    string chatUrl;
    string promptUrl;
    string slotUrl;
    int verbosity;
    int timeoutS;
    string apiKey;

    struct Chat {
        string model;
        long max_tokens;
        double temperature;
        long reasoning_budget;
        bool preserve_thinking;
    }

    Chat chat;

    /// Maximum number of retry attempts for HTTP requests (default: 3)
    int maxRetries = 3;

    /// Base backoff in milliseconds for exponential backoff (default: 500)
    long backoffBaseMs = 500;
}

/// Error representation for HTTP POST failures (non-throwing).
struct HttpPostError {
    int statusCode; // HTTP status code (0 if no response / network error)
    string body; // Response body or error message
    string errorMsg; // Human-readable error description
}

struct LlamaRequestError {
    int code;
    string response;
}

struct LlmRequester {
    RequestConfig cfg;
    Nullable!JSONValue tools;
    void delegate(string msg) requestLog;
    void delegate(string msg) replyLog;

    private {
        Request rq;
    }

    SumType!(JSONValue, LlamaRequestError) request(Chat chat) nothrow {
        try {
            auto headers = ["Content-Type": "application/json"];
            if (!cfg.apiKey.empty)
                headers["Authorization"] = "Bearer " ~ cfg.apiKey;

            auto jsonReq = chat.toJson.addConfig(cfg.chat);
            if (!tools.isNull) {
                jsonReq["tools"] = tools.get;
            }
            if (requestLog)
                requestLog(jsonReq.toPrettyString);
            if (cfg.verbosity >= 2)
                logger.trace(jsonReq.toPrettyString);

            rq.verbosity(cfg.verbosity);

            auto result = httpPostWithRetry(rq, cfg.chatUrl, jsonReq.toString,
                    headers, cfg.maxRetries, cfg.timeoutS, cfg.backoffBaseMs);

            return result.match!((HttpPostResult r) {
                if (replyLog)
                    replyLog(r.body);
                if (r.statusCode == 200) {
                    return SumType!(JSONValue, LlamaRequestError)(parseJSON(r.body));
                }
                return SumType!(JSONValue, LlamaRequestError)(LlamaRequestError(r.statusCode,
                    r.body));
            }, (HttpPostError e) {
                if (e.statusCode == 0) {
                    logger.trace(e.errorMsg);
                }
                return SumType!(JSONValue, LlamaRequestError)(LlamaRequestError(e.statusCode,
                    e.body));
            });
        } catch (Exception e) {
            logger.trace(e.msg).collectException;
        }
        return SumType!(JSONValue, LlamaRequestError)(LlamaRequestError(404, "exception thrown"));
    }
}

struct LlmSlotRequester {
    string slotUrl;
    string apiKey;
    string model;
    RequestConfig.Chat chat;

    private {
        Request rq;
    }

    SumType!(JSONValue, LlamaRequestError) request() nothrow {
        try {
            auto headers = ["Content-Type": "application/json"];
            if (!apiKey.empty)
                headers["Authorization"] = "Bearer " ~ apiKey;
            if (!model.empty)
                headers["model"] = model;

            auto result = httpGetWithRetry(rq, slotUrl, headers, 3, 60, 500);

            return result.match!((HttpPostResult r) {
                if (r.statusCode == 200) {
                    return SumType!(JSONValue, LlamaRequestError)(parseJSON(r.body));
                }
                return SumType!(JSONValue, LlamaRequestError)(LlamaRequestError(r.statusCode,
                    r.body));
            }, (HttpPostError e) {
                if (e.statusCode == 0) {
                    logger.trace(e.errorMsg);
                }
                return SumType!(JSONValue, LlamaRequestError)(LlamaRequestError(e.statusCode,
                    e.body));
            });
        } catch (Exception e) {
            logger.trace(e.msg).collectException;
        }
        return SumType!(JSONValue, LlamaRequestError)(LlamaRequestError(404, "exception thrown"));
    }

    long request(long fallbackContext) nothrow {
        try {
            return request().match!((JSONValue j) {
                logger.trace(j.toPrettyString);
                if (auto ctx = "n_ctx" in j)
                    return ctx.integer;
                return fallbackContext;
            }, (LlamaRequestError e) {
                logger.tracef("unable to get the context size. Using fallback value %s: %s",
                    fallbackContext, e);
                return fallbackContext;
            });
        } catch (Exception e) {
            logger.tracef("unable to get the context size. Using fallback value %s: %s",
                    fallbackContext, e.msg).collectException;
        }
        return fallbackContext;
    }
}

JSONValue addConfig(JSONValue j, RequestConfig.Chat cfg) {
    import std.math : isNaN;
    import std.array : empty;

    if (!cfg.model.empty)
        j["model"] = cfg.model;
    if (!cfg.temperature.isNaN)
        j["temperature"] = cfg.temperature;
    if (cfg.max_tokens != 0)
        j["max_tokens"] = cfg.max_tokens;
    if (cfg.reasoning_budget != 0 || cfg.preserve_thinking) {
        j["chat_template_kwargs"] = JSONValue.emptyObject;
        if (cfg.reasoning_budget != 0)
            j["chat_template_kwargs"]["reasoning_budget"] = cfg.reasoning_budget;
        if (cfg.preserve_thinking)
            j["chat_template_kwargs"]["preserve_thinking"] = true;
    }

    return j;
}

/// Result from an HTTP POST request.
struct HttpPostResult {
    int statusCode;
    string body;
}

/// Execute an HTTP POST with retry and exponential backoff.
/// Returns SumType: HttpPostResult on success, HttpPostError on failure.
SumType!(HttpPostResult, HttpPostError) httpPostWithRetry(Request rq, string url, string body,
        string[string] headers, long maxRetries, long timeoutSeconds, long backoffBaseMs = 500) {
    import std.algorithm : canFind;
    import std.conv : to;

    rq.addHeaders(headers);
    rq.timeout = timeoutSeconds.dur!"seconds";

    int attempt = 0;
    HttpPostError lastError;

    while (attempt <= maxRetries) {
        if (attempt > 0) {
            long backoff = backoffBaseMs * (1L << (attempt - 1));
            Thread.sleep(backoff.dur!"msecs");
        }

        attempt++;
        try {
            auto rs = rq.exec!"POST"(url, body);
            auto response = (cast(const(char)[])(rs.responseBody)).byUTF!char.text;
            int code = rs.code;

            if (code >= 500) {
                lastError = HttpPostError(code, response,
                        "HTTP " ~ code.to!string ~ " (server error, retryable): " ~ response);
                continue;
            }
            if (code >= 400) {
                return SumType!(HttpPostResult, HttpPostError)(HttpPostError(code, response,
                        "HTTP " ~ code.to!string ~ " (client error, not retryable): " ~ response));
            }
            return SumType!(HttpPostResult, HttpPostError)(HttpPostResult(code, response));
        } catch (Exception e) {
            lastError = HttpPostError(0, "", e.msg);
            bool isRetryable = false;
            if (canFind("HTTP 5", lastError.errorMsg))
                isRetryable = true;
            if (canFind("timeout", lastError.errorMsg))
                isRetryable = true;
            if (canFind("connection", lastError.errorMsg))
                isRetryable = true;
            if (canFind("refused", lastError.errorMsg))
                isRetryable = true;
            if (!isRetryable) {
                return SumType!(HttpPostResult, HttpPostError)(lastError);
            }
            if (attempt > maxRetries) {
                return SumType!(HttpPostResult, HttpPostError)(lastError);
            }
        }
    }
    return SumType!(HttpPostResult, HttpPostError)(lastError);
}

/// Execute an HTTP GET with retry and exponential backoff.
/// Returns SumType: HttpPostResult on success, HttpPostError on failure.
SumType!(HttpPostResult, HttpPostError) httpGetWithRetry(Request rq, string url,
        string[string] headers, int maxRetries, int timeoutSeconds, long backoffBaseMs = 500) {
    import std.algorithm : canFind;
    import std.conv : to;

    rq.addHeaders(headers);
    rq.timeout = timeoutSeconds.dur!"seconds";

    int attempt = 0;
    HttpPostError lastError;

    while (attempt <= maxRetries) {
        if (attempt > 0) {
            long backoff = backoffBaseMs * (1L << (attempt - 1));
            Thread.sleep(backoff.dur!"msecs");
        }

        attempt++;
        try {
            auto rs = rq.exec!"GET"(url);
            auto response = (cast(const(char)[])(rs.responseBody)).byUTF!char.text;
            int code = rs.code;

            if (code >= 500) {
                lastError = HttpPostError(code, response,
                        "HTTP " ~ code.to!string ~ " (server error, retryable): " ~ response);
                continue;
            }
            if (code >= 400) {
                return SumType!(HttpPostResult, HttpPostError)(HttpPostError(code, response,
                        "HTTP " ~ code.to!string ~ " (client error, not retryable): " ~ response));
            }
            return SumType!(HttpPostResult, HttpPostError)(HttpPostResult(code, response));
        } catch (Exception e) {
            lastError = HttpPostError(0, "", e.msg);
            bool isRetryable = false;
            if (canFind("HTTP 5", lastError.errorMsg))
                isRetryable = true;
            if (canFind("timeout", lastError.errorMsg))
                isRetryable = true;
            if (canFind("connection", lastError.errorMsg))
                isRetryable = true;
            if (canFind("refused", lastError.errorMsg))
                isRetryable = true;
            if (!isRetryable) {
                return SumType!(HttpPostResult, HttpPostError)(lastError);
            }
            if (attempt > maxRetries) {
                return SumType!(HttpPostResult, HttpPostError)(lastError);
            }
        }
    }
    return SumType!(HttpPostResult, HttpPostError)(lastError);
}
