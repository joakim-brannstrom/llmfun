module llm.query;

import core.thread : Thread;
import core.time : dur, Duration;
import logger = std.logger;
import std.array : empty;
import std.conv : to, text;
import std.exception : collectException;
import std.json : JSONValue, parseJSON, JSONType, JSONOptions;
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
    bool keepAlive = true;
    bool verifySslCert = true;
    string apiKey;

    JSONValue header;

    /// Maximum number of retry attempts for HTTP requests (default: 3)
    int maxRetries = 3;

    /// Base backoff in milliseconds for exponential backoff (default: 500)
    long backoffBaseMs = 500;
}

/// Error representation for HTTP POST failures (non-throwing).
struct HttpError {
    int statusCode; // HTTP status code (0 if no response / network error)
    string body; // Response body or error message
    string errorMsg; // Human-readable error description
}

struct HttpResult {
    int statusCode;
    string body;
}

struct LlamaRequestError {
    int code;
    string response;
}

struct LlmRequester {
    RequestConfig cfg;
    Nullable!JSONValue tools;

    private {
        LibRequestConfig rqCfg;
        Request rq;
    }

    this(RequestConfig cfg) {
        this(cfg, Nullable!JSONValue.init);
    }

    this(RequestConfig cfg, Nullable!JSONValue tools) {
        this.cfg = cfg;
        this.tools = tools;

        auto headers = ["Content-Type": "application/json"];
        if (!cfg.apiKey.empty)
            headers["Authorization"] = "Bearer " ~ cfg.apiKey;
        this.rqCfg = LibRequestConfig(headers: headers, maxRetries: cfg.maxRetries, timeout: cfg.timeoutS
                .dur!"seconds", sslSetVerifyPeer: cfg.verifySslCert, backoffBaseMs: cfg.backoffBaseMs,
                verbosity: cfg.verbosity, keepAlive: cfg.keepAlive);
    }

    void setCallbacks(void delegate(const(char)[]) stream, bool delegate() interrupt) {
        rqCfg.stream = stream;
        rqCfg.interrupt = interrupt;
    }

    SumType!(HttpResult, HttpError) request(Chat chat) nothrow {
        alias ReturnT = typeof(return);

        try {
            auto jsonReq = chat.toJson.merge(cfg.header);
            if (!tools.isNull) {
                jsonReq["tools"] = tools.get;
            }
            if (rqCfg.stream !is null) {
                jsonReq["stream"] = true;
            }
            if (cfg.verbosity >= 2)
                logger.trace(jsonReq.toPrettyString);

            return httpPostWithRetry(rq, cfg.chatUrl,
                    jsonReq.toString(JSONOptions.doNotEscapeSlashes), rqCfg);
        } catch (Exception e) {
            logger.trace(e.msg).collectException;
        }
        return ReturnT(HttpError(statusCode: 404, body: "", errorMsg: "exception thrown"));
    }
}

SumType!(JSONValue, LlamaRequestError) toJson(SumType!(HttpResult, HttpError) result) {
    alias ReturnT = typeof(return);
    try {
        return result.match!((HttpResult r) {
            if (r.statusCode == 200) {
                return ReturnT(parseJSON(r.body));
            }
            return ReturnT(LlamaRequestError(r.statusCode, r.body));
        }, (HttpError e) {
            if (e.statusCode == 0) {
                logger.trace(e.errorMsg);
            }
            return ReturnT(LlamaRequestError(e.statusCode, e.body));
        });
    } catch (Exception e) {
        logger.trace(e.msg).collectException;
    }
    return ReturnT(LlamaRequestError(404, "exception thrown"));
}

long[string] LlmSlotRequesterCache;
struct LlmSlotRequester {
    RequestConfig cfg;

    private {
        LibRequestConfig rqCfg;
        Request rq;
    }

    this(RequestConfig cfg) {
        this.cfg = cfg;

        auto headers = ["Content-Type": "application/json"];
        if (!cfg.apiKey.empty)
            headers["Authorization"] = "Bearer " ~ cfg.apiKey;
        this.rqCfg = LibRequestConfig(headers: headers, maxRetries: cfg.maxRetries, timeout: cfg.timeoutS
                .dur!"seconds", sslSetVerifyPeer: cfg.verifySslCert, backoffBaseMs: cfg.backoffBaseMs,
                verbosity: cfg.verbosity, keepAlive: cfg.keepAlive);
    }

    SumType!(JSONValue, LlamaRequestError) request() nothrow {
        alias ReturnT = typeof(return);

        try {
            auto url = cfg.slotUrl;
            if (auto model = "model" in cfg.header)
                url ~= i"?model=$(model.str)".text;

            auto result = httpGetWithRetry(rq, url, rqCfg);

            return result.match!((HttpResult r) {
                if (r.statusCode == 200) {
                    return ReturnT(parseJSON(r.body));
                }
                return ReturnT(LlamaRequestError(r.statusCode, r.body));
            }, (HttpError e) {
                if (e.statusCode == 0) {
                    logger.tracef("http error: url:'%s' msg:'%s'", cfg.slotUrl, e.errorMsg);
                }
                return ReturnT(LlamaRequestError(e.statusCode, e.body));
            });
        } catch (Exception e) {
            logger.trace(e.msg).collectException;
        }
        return ReturnT(LlamaRequestError(404, "exception thrown"));
    }

    long request(long fallbackContext) nothrow {
        string model;
        try {
            if (auto a = "model" in cfg.header)
                model = a.str;
        } catch (Exception e) {
        }
        if (auto v = model in LlmSlotRequesterCache) {
            return *v;
        }

        try {
            return request().match!((JSONValue j) {
                if (cfg.verbosity >= 2)
                    logger.trace(j.toPrettyString);
                if (j.type == JSONType.array) {
                    const v = j[0]["n_ctx"].integer;
                    LlmSlotRequesterCache[model] = v;
                    return v;
                } else if ("n_ctx" in j) {
                    const v = j["n_ctx"].integer;
                    LlmSlotRequesterCache[model] = v;
                    return v;
                }
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

struct LibRequestConfig {
    bool delegate() interrupt;
    void delegate(const(char)[] data) stream;

    string[string] headers;
    long maxRetries = 3;
    Duration timeout = 3600.dur!"seconds";
    bool sslSetVerifyPeer = true;
    long backoffBaseMs = 500;
    long verbosity;
    bool keepAlive;

    bool isConfigured;

    void conf(ref Request rq) {
        if (isConfigured)
            return;
        isConfigured = true;
        reconfigure(rq);
    }

    void reconfigure(ref Request rq) {
        rq.addHeaders(headers);
        rq.timeout = timeout;
        rq.sslSetVerifyPeer = sslSetVerifyPeer;
        rq.verbosity = cast(uint) verbosity;
        rq.keepAlive = keepAlive;
        rq.useStreaming = true;
        isConfigured = true;
    }
}

/// Execute an HTTP request with retry and exponential backoff.
SumType!(HttpResult, HttpError) httpWithRetry(string HttpReqType)(ref Request rq,
        string url, string body, ref LibRequestConfig cfg) {
    import std.algorithm : canFind;
    import std.array : appender;
    import std.conv : to;
    import std.utf : validate;
    import llm.utility : isSignalSIGPIPETriggered, clearSignalSIGPIPE;

    alias ReturnT = typeof(return);

    cfg.conf(rq);

    int attempt = 0;
    HttpError lastError;

    while (attempt <= cfg.maxRetries) {
        if (attempt > 0) {
            long backoff = cfg.backoffBaseMs * (1L << (attempt - 1));
            Thread.sleep(backoff.dur!"msecs");
        }
        if (isSignalSIGPIPETriggered) {
            logger.trace("SIGPIPE detected. Resetting Request instance");
            rq = Request();
            cfg.reconfigure(rq);
            clearSignalSIGPIPE;
        }

        attempt++;
        try {
            static if (HttpReqType == "POST")
                auto rs = rq.exec!HttpReqType(url, body);
            else static if (HttpReqType == "GET")
                auto rs = rq.exec!HttpReqType(url);
            else
                static assert(0, "Unknown request type: " ~ HttpReqType);
            const int code = rs.code;

            auto streamData = appender!(const(ubyte)[])();
            StreamByLine sbl;
            sbl.receive(rs, (const(ubyte)[] chunk) {
                if (cfg.interrupt !is null && cfg.interrupt()) {
                    logger.trace("user interrupted stream");
                    return false;
                }
                if (chunk.empty) {
                    return true;
                }
                if (cfg.stream is null) {
                    streamData.put(chunk);
                } else {
                    const(char)[] str;
                    try {
                        validate(cast(const(char)[]) chunk);
                        str = cast(const(char)[]) chunk;
                    } catch (Exception e) {
                    }
                    if (!str.empty)
                        cfg.stream(str);
                }
                return true;

            });
            auto response = (cast(const(char)[])(streamData[])).byUTF!char.text;

            if (code >= 500) {
                lastError = HttpError(code, response, i"HTTP $(code) (server error, retryable): $(
                        response)".text);
                continue;
            }
            if (code >= 400) {
                return ReturnT(HttpError(code, response, i"HTTP $(code) (client error, not retryable): $(
                        response)".text));
            }
            return ReturnT(HttpResult(code, response));
        } catch (Exception e) {
            lastError = HttpError(0, "", e.msg);
            rq = Request();
            cfg.reconfigure(rq);
        }
    }
    return ReturnT(lastError);
}

/// Execute an HTTP POST with retry and exponential backoff.
SumType!(HttpResult, HttpError) httpPostWithRetry(ref Request rq, string url,
        string body, ref LibRequestConfig cfg) {
    return httpWithRetry!"POST"(rq, url, body, cfg);
}

/// Execute an HTTP POST with retry and exponential backoff.
SumType!(HttpResult, HttpError) httpGetWithRetry(ref Request rq, string url,
        ref LibRequestConfig cfg) {
    return httpWithRetry!"GET"(rq, url, "", cfg);
}

private:

struct StreamByLine {
    const(ubyte)[] app;

    void receive(ref Response rs, bool delegate(const(ubyte)[] data) callback) {
        auto stream = rs.receiveAsRange();
        try {
            while (!stream.empty) {
                app ~= stream.front;
                stream.popFront;

                for (size_t pos; pos < app.length; ++pos) {
                    if (app[pos] == '\n') {
                        if (!callback(app[0 .. pos])) {
                            return;
                        }
                        app = app[pos + 1 .. $];
                        pos = 0;
                    }
                }
            }
        } catch (Exception e) {
        }

        for (size_t pos; pos < app.length; ++pos) {
            if (app[pos] == '\n') {
                callback(app[0 .. pos]);
                app = app[pos + 1 .. $];
                pos = 0;
            }
        }
        if (!app.empty) {
            callback(app);
        }
    }
}

/// Merge y into x and return the result
JSONValue merge(JSONValue x, JSONValue y) {
    foreach (kv; y.object.byKeyValue) {
        x[kv.key] = kv.value;
    }
    return x;
}
