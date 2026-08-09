module llm.query;

import core.thread : Thread;
import core.time : dur, Duration;
import etc.c.curl : CurlError;
import logger = std.logger;
import std.array : empty;
import std.conv : to, text;
import std.exception : collectException;
import std.json : JSONValue, parseJSON, JSONType, JSONOptions;
import std.net.curl;
import std.sumtype : SumType, match;
import std.typecons : Nullable;
import std.utf : byUTF, validate, UTFException;

import llm.chat;

struct RequestConfig {
    string chatUrl;
    string promptUrl;
    string slotUrl;
    int verbosity;
    int timeoutS;
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
    }

    this(RequestConfig cfg) {
        this(cfg, Nullable!JSONValue.init);
    }

    this(RequestConfig cfg, Nullable!JSONValue tools) {
        this.cfg = cfg;
        this.tools = tools;

        auto headers = string[string].init;
        if (!cfg.apiKey.empty)
            headers["Authorization"] = "Bearer " ~ cfg.apiKey;
        this.rqCfg = LibRequestConfig(headers: headers, maxRetries: cfg.maxRetries, timeout: cfg
                .timeoutS.dur!"seconds", sslSetVerifyPeer: cfg.verifySslCert,
                backoffBaseMs: cfg.backoffBaseMs, verbosity: cfg.verbosity);
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

            return httpPostWithRetry(cfg.chatUrl,
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
    }

    this(RequestConfig cfg) {
        this.cfg = cfg;

        auto headers = string[string].init;
        if (!cfg.apiKey.empty)
            headers["Authorization"] = "Bearer " ~ cfg.apiKey;
        this.rqCfg = LibRequestConfig(headers: headers, maxRetries: cfg.maxRetries, timeout: cfg
                .timeoutS.dur!"seconds", sslSetVerifyPeer: cfg.verifySslCert,
                backoffBaseMs: cfg.backoffBaseMs, verbosity: cfg.verbosity);
    }

    SumType!(JSONValue, LlamaRequestError) request() nothrow {
        alias ReturnT = typeof(return);

        try {
            auto url = cfg.slotUrl;
            if (auto model = "model" in cfg.header)
                url ~= i"?model=$(model.str)".text;

            auto result = httpGetWithRetry(url, rqCfg);

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

    /// Configure a fresh std.net.curl HTTP instance with the static settings.
    /// URL, method and POST body are set per call in httpWithRetry.
    void applyTo(ref HTTP http) {
        http.operationTimeout = timeout;
        http.connectTimeout = timeout;
        http.verifyPeer = sslSetVerifyPeer;
        http.verbose = (verbosity >= 1);
        foreach (key, value; headers)
            http.addRequestHeader(key, value);
    }
}

/// Execute an HTTP request with retry and exponential backoff.
SumType!(HttpResult, HttpError) httpWithRetry(string HttpReqType)(string url,
        string body, ref LibRequestConfig cfg) {
    alias ReturnT = typeof(return);

    int attempt = 0;
    HttpError lastError;

    while (attempt <= cfg.maxRetries) {
        if (attempt > 0) {
            long backoff = cfg.backoffBaseMs * (1L << (attempt - 1));
            Thread.sleep(backoff.dur!"msecs");
        }

        attempt++;
        try {
            // Note: 3xx redirects are NOT followed (std.net.curl has no
            // redirect support). A redirect response is returned as-is, so
            // configured endpoints must not redirect.
            HTTP http = HTTP();
            cfg.applyTo(http);
            http.url = url;
            static if (HttpReqType == "POST") {
                http.method = HTTP.Method.post;
                // setPostData adds a Content-Type header itself, so the
                // configured header map must NOT contain one (curl would
                // send it twice).
                http.setPostData(body, "application/json");
            } else static if (HttpReqType == "GET") {
                http.method = HTTP.Method.get;
            } else {
                static assert(0, "Unknown request type: " ~ HttpReqType);
            }

            auto sbl = StreamByLine(cfg.stream, cfg.interrupt);
            http.onReceive = (ubyte[] data) {
                if (sbl.checkInterrupt()) {
                    logger.trace("user interrupted stream");
                    return 0;
                }
                sbl.feed(data);
                return data.length;
            };

            auto curlCode = http.perform(ThrowOnError.no);
            // On user interrupt, stop immediately: no partial-line flush and
            // no retry. Otherwise flush any trailing partial line first.
            if (sbl.checkInterrupt())
                return ReturnT(HttpError(0, "", "user interrupted stream"));
            sbl.flush();
            if (curlCode != CurlError.ok) {
                // Full messages would require linking libcurl directly
                // (etc.c.curl.curl_easy_strerror), which the project avoids
                // because std.net.curl loads libcurl dynamically. Map the
                // common codes to readable text, fall back to the number.
                lastError = HttpError(0, "", curlErrorText(curlCode));
                continue;
            }

            const int code = http.statusLine.code;
            string response;
            try {
                response = (cast(const(char)[])(sbl.fullResponse)).byUTF!char.text;
            } catch (UTFException e) {
                // Malformed body on a completed transfer: keep the HTTP status
                // instead of retrying a request that already got a response.
                logger.tracef("invalid UTF-8 in response body: %s", e.msg);
                response = "";
            }

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
        }
    }
    return ReturnT(lastError);
}

/// Execute an HTTP POST with retry and exponential backoff.
SumType!(HttpResult, HttpError) httpPostWithRetry(string url, string body, ref LibRequestConfig cfg) {
    return httpWithRetry!"POST"(url, body, cfg);
}

/// Execute an HTTP GET with retry and exponential backoff.
SumType!(HttpResult, HttpError) httpGetWithRetry(string url, ref LibRequestConfig cfg,
        string body = "") {
    return httpWithRetry!"GET"(url, body, cfg);
}

/// Human-readable text for common curl error codes. std.net.curl loads
/// libcurl dynamically, so curl_easy_strerror is not available without
/// linking libcurl directly; this covers the codes users actually hit and
/// falls back to the numeric code.
private string curlErrorText(int code) {
    static immutable string[int] msgs = [
        6: "could not resolve host", 7: "could not connect",
        28: "operation timed out", 42: "transfer aborted",
    ];
    if (auto p = code in msgs)
        return *p;
    return to!string(code);
}

private:

/// Line-buffering helper for std.net.curl's onReceive callback. Accumulates
/// raw response bytes, splits on '\n', validates UTF-8 and forwards complete
/// lines (without the trailing newline) to the stream delegate.
struct StreamByLine {
    private {
        void delegate(const(char)[]) stream;
        bool delegate() interrupt;
        const(ubyte)[] app; // working buffer for line splitting
        const(ubyte)[] full; // complete raw response body
    }

    this(void delegate(const(char)[]) stream, bool delegate() interrupt) {
        this.stream = stream;
        this.interrupt = interrupt;
    }

    /// Returns true if the interrupt delegate is set and requests an abort.
    bool checkInterrupt() {
        return interrupt !is null && interrupt();
    }

    /// Accumulate a chunk of raw response bytes and deliver complete lines
    /// (ending in '\n') to the stream delegate.
    void feed(const(ubyte)[] chunk) {
        // Note: the full body is buffered in memory even when streaming, so
        // the response body stays available for the HttpResult. An unbounded
        // stream will grow without limit.
        full ~= chunk;
        app ~= chunk;
        size_t pos = 0;
        while (pos < app.length) {
            if (app[pos] == '\n') {
                deliver(app[0 .. pos]);
                app = app[pos + 1 .. $];
                pos = 0; // re-check index 0 (handles consecutive '\n')
            } else {
                ++pos;
            }
        }
    }

    /// Deliver any remaining partial line (no trailing '\n') after the
    /// transfer has finished.
    void flush() {
        if (!app.empty) {
            deliver(app);
            app = null;
        }
    }

    /// The complete raw response body accumulated so far.
    const(ubyte)[] fullResponse() const {
        return full;
    }

    private void deliver(const(ubyte)[] line) {
        if (stream is null)
            return;
        const(char)[] str;
        try {
            validate(cast(const(char)[]) line);
            str = cast(const(char)[]) line;
        } catch (Exception e) {
            // Invalid UTF-8: drop the line instead of crashing the transfer.
            // Deliberate asymmetry with httpWithRetry, where a malformed
            // response body (built from fullResponse) fails wholesale and
            // yields an empty body.
        }
        if (!str.empty)
            stream(str);
    }
}

/// Merge y into x and return the result
JSONValue merge(JSONValue x, JSONValue y) {
    foreach (kv; y.object.byKeyValue) {
        x[kv.key] = kv.value;
    }
    return x;
}
