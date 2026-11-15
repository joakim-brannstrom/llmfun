module llm.utility;

import logger = std.logger;
import std.algorithm : sort, filter;
import std.array : array, appender, empty, join;
import std.conv : text;
import std.format : formattedWrite;
import std.json : JSONValue, JSONType, parseJSON, JSONOptions;
import std.stdio : writef, stdout;
import std.utf : byUTF, toUTF8, validate, UTFException;

import my.path;

import llm.chat : Role, ToolResponse;

/// Read the file and sanitize its UTF-8: each invalid sequence is
/// replaced with U+FFFD. Unlike std.file.readText, this never throws
/// UTFException (unreadable files still throw FileException). Valid files
/// are returned without copying, so content hashes stay stable across
/// re-syncs. @trusted: the fast path casts const bytes to `string`
/// (see sanitizeUtf8).
string readFileUtf8(Path p) @trusted {
    import std.file : read;

    return (cast(string) read(p)).sanitizeUtf8;
}

/// Replaces invalid UTF-8 sequences with U+FFFD (Unicode replacement character).
/// Fast path: returns s unchanged (same allocation) if it is already valid.
/// Never throws; idempotent (U+FFFD is itself valid UTF-8).
/// @trusted: the fast path returns the input bytes as `string` (immutable),
/// which requires a const-to-immutable cast that @safe disallows.
T sanitizeUtf8(T)(T s) @safe nothrow {
    if (s.empty)
        return s;
    try {
        validate(s); // O(n) scan, throws UTFException on the first bad byte
        return s;
    } catch (Exception) {
        // byUTF!char on a char[] is a byte pass-through (no validation);
        // decoding to dchar replaces invalid sequences with U+FFFD.
        static if (is(T == string))
            return s.byUTF!dchar.toUTF8;
        else
            return s.byUTF!dchar.toUTF8.dup;
    }
}

@("sanitizeUtf8 returns an empty string unchanged")
unittest {
    assert(sanitizeUtf8("") == "");
}

@("sanitizeUtf8 returns valid ASCII unchanged without copying")
unittest {
    string s = "hello world";
    assert(sanitizeUtf8(s) is s);
}

@("sanitizeUtf8 returns valid multibyte UTF-8 unchanged without copying")
unittest {
    string s = "héllo wörld ☕";
    assert(sanitizeUtf8(s) is s);
}

@("sanitizeUtf8 replaces a lone 0xF0 incomplete 4-byte sequence")
unittest {
    assert(sanitizeUtf8("\xF0") == "\uFFFD");
}

@("sanitizeUtf8 replaces a truncated 3-byte sequence")
unittest {
    assert(sanitizeUtf8("\xE2\x82") == "\uFFFD");
}

@("sanitizeUtf8 replaces malformed 0xFF sequences with U+FFFD")
unittest {
    assert(sanitizeUtf8("\xFF") == "\uFFFD");
    // Two adjacent 0xFF bytes form one malformed sequence (lead byte +
    // continuation byte), so they yield a single U+FFFD.
    assert(sanitizeUtf8("\xFF\xFF") == "\uFFFD");
    assert(sanitizeUtf8("\xFF\xFF\xFF") == "\uFFFD\uFFFD");
    // 0xFF is an invalid lead byte; the decoder consumes the following
    // byte as part of the malformed sequence, so 'c' is dropped too.
    assert(sanitizeUtf8("ab\xFFcd") == "ab\uFFFDd");
}

@("sanitizeUtf8 preserves valid content around invalid bytes")
unittest {
    assert(sanitizeUtf8("ab\x80cd") == "ab\uFFFDcd");
}

@("sanitizeUtf8 is idempotent for input already containing U+FFFD")
unittest {
    string s = "a\uFFFDb";
    assert(sanitizeUtf8(s) is s);
    assert(sanitizeUtf8(sanitizeUtf8("\xF0")) == sanitizeUtf8("\xF0"));
}

// Convert a 4-byte hash to a long (little-endian byte order).
private long toLong(ubyte[4] a) @safe {
    return a[0] | a[1] << 8 | a[2] << 16 | a[3] << 24;
}

// Compute a content checksum using MurmurHash3-32.
// Params:
//  content The text content to hash
// Returns: A long representing the 32-bit hash value
long computeContentHash(string content) @safe {
    import std.digest : digest;
    import std.digest.murmurhash : MurmurHash3;

    return toLong(digest!(MurmurHash3!32)(content));
}

string compressionResultToString(bool compressed, size_t originalLength,
        size_t newLength, size_t keptXCount, long keptXTokens, long ctxUsed, long newContextSize) {
    if (compressed) {
        return i"Compression finished. History $(originalLength)->$(newLength) messages, kept $(
                keptXCount) msgs ($(keptXTokens) tokens), context $(ctxUsed)->$(newContextSize)\n"
            .text;
    }
    return null;
}

string summarizeToolResponse(ToolResponse msg, size_t maxLength) {
    auto content = msg.content;

    try {
        content = parseJSON(msg.content).toPrettyString(JSONOptions.doNotEscapeSlashes);
    } catch (Exception e) {
    }
    return content.length < maxLength ? content : i"$(content[0 .. maxLength])... ($(content.length) chars)"
        .text;
}

string[] summarizeToolCalls(JSONValue calls, size_t maxLength) @trusted {
    const errorMsg = "Wants to run: <unknown>";
    if (calls.type != JSONType.array || calls.array.empty)
        return [errorMsg];

    string process(JSONValue call) {
        try {
            if ("function" !in call)
                return errorMsg;
            call = call["function"];

            auto buf = appender!string();

            formattedWrite(buf, "%-s(", call["name"].str);

            auto args = parseJSON(call["arguments"].str);

            buf.put(summarizeToolCallArguments(args, maxLength));

            buf.put(")");
            return buf[];
        } catch (Exception e) {
            logger.trace("summary failed, should not happen: ", e.msg);
            return "Wants to run: <unknown>";
        }
    }

    auto rval = appender!(string[])();
    foreach (call; calls.array) {
        rval.put(process(call));
    }

    return rval[];
}

string summarizeToolCallArguments(JSONValue args, size_t maxLength) @trusted {
    try {
        auto buf = appender!string();

        string[] params;
        foreach (key, value; args.object) {
            auto valueStr = value.toString;
            if (valueStr.length > maxLength) {
                valueStr = i"'$(valueStr[0 .. maxLength])...' ($(valueStr.length) chars)".text;
            }
            params ~= i"$(key)=$(valueStr)".text;
        }

        // Sort parameters by length so shortest (most visible) come first
        params = sort!("a.length < b.length")(params).array;
        bool isFirst = true;
        foreach (p; params) {
            if (!isFirst)
                buf.put(",\n");
            buf.put(p);
            isFirst = false;
        }
        return buf[];
    } catch (Exception e) {
        logger.trace("summary failed, should not happen: ", e.msg);
        return null;
    }
}

// The tool result's display payload: the sorted, capped key/value pair set for
// a JSON-object result, or the fallback one-line string for anything else. The
// caps live here - the store and the view take the payload as it arrives.
struct ToolResultPairs {
    string[] keys;
    string[] values;
    uint more; // pairs past the pair cap, 0 when under
    string raw; // the one-line fallback string, non-object results only
}

private enum uint kMaxResultPairs = 100;
private enum size_t kMaxPairValueCodepoints = 200;

/// Whitespace runs collapse to single spaces, byte-wise: the same shape the
/// C-side oneLine gives today's dim RESULT line, so a receiver rendering the
/// fallback is byte-identical to its render of that line.
private string resultOneLine(string s) @safe {
    auto buf = appender!string();
    bool pendingSpace;
    foreach (char c; s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            pendingSpace = buf.data.length != 0;
            continue;
        }
        if (pendingSpace) {
            buf.put(' ');
            pendingSpace = false;
        }
        buf.put(c);
    }
    return buf[];
}

/// A pair value's text: strings raw, nested containers as compact JSON, other
/// scalars via their JSON text.
private string pairValueText(JSONValue value) @safe {
    if (value.type == JSONType.string)
        return value.str;
    return value.toString(JSONOptions.doNotEscapeSlashes);
}

/// The value cut to the codepoint cap: a multibyte character at the cut stays
/// whole, so a receiver's strings are never invalid UTF-8.
private string capPairValue(string value) @safe {
    size_t count;
    foreach (size_t i, dchar c; value) {
        if (count == kMaxPairValueCodepoints)
            return value[0 .. i];
        count++;
    }
    return value;
}

/// Unpack a tool result's payload into its display pairs. A JSON object walks
/// one level (nested objects/arrays stringify as compact JSON, scalars as
/// text); pairs sort byte-wise by key; duplicate keys keep the last value. A
/// non-JSON payload, an array, or an object with no pairs keeps the pairs
/// empty and carries the one-line fallback string instead. A cancelled call
/// never reaches this - the caller skips it.
ToolResultPairs unpackToolResultPairs(ToolResponse resp) {
    ToolResultPairs out_;

    string fallback() {
        return resultOneLine(summarizeToolResponse(resp, 1000));
    }

    JSONValue parsed;
    try {
        parsed = parseJSON(resp.content);
    } catch (Exception e) {
        out_.raw = fallback();
        return out_;
    }
    if (parsed.type != JSONType.object) {
        out_.raw = fallback();
        return out_;
    }

    string[] keys;
    foreach (key, _; parsed.object)
        keys ~= key;
    sort(keys);

    foreach (key; keys) {
        if (out_.keys.length == kMaxResultPairs)
            break;
        out_.keys ~= key;
        out_.values ~= capPairValue(pairValueText(parsed.object[key]));
    }
    out_.more = cast(uint)(parsed.object.length > kMaxResultPairs
            ? parsed.object.length - kMaxResultPairs : 0);
    if (out_.keys.empty)
        out_.raw = fallback();
    return out_;
}

void configCatchCtrlC() {
    import core.stdc.signal;

    signal(SIGINT, &handleSIGINT);
}

private shared bool signalStopAgent;

void stopAgent() nothrow @nogc @safe {
    .signalStopAgent = true;
}

bool isStopAgentTriggered() @safe nothrow @nogc {
    return .signalStopAgent;
}

void clearStopAgent() @safe nothrow @nogc {
    .signalStopAgent = false;
}

private shared bool signalInterrupt;
private extern (C) void handleSIGINT(int sig) nothrow @nogc @system {
    .signalInterrupt = true;
}

bool isInterruptTriggered() @safe nothrow @nogc {
    return .signalInterrupt;
}

void clearInterruptSignal() @safe nothrow @nogc {
    .signalInterrupt = false;
}

T getValue(T)(JSONValue v, T delegate(JSONValue v) accessor, T default_) @trusted {
    try {
        return accessor(v);
    } catch (Exception e) {
        return default_;
    }
}

// Check if a path is safely inside the workarea.
// Uses proper directory boundary check to prevent prefix attacks
// (e.g., /workarea_evil would not match /workarea).
// Params:
//  path The path to check
//  workArea The workarea root path
// Returns: true if path is safely inside workarea
bool isPathInsideWorkarea(AbsolutePath path, AbsolutePath workArea) @safe pure nothrow @nogc {
    import std.algorithm : startsWith;

    if (!path.toString.startsWith(workArea.toString))
        return false;
    // Ensure proper directory boundary: either exact match or next char is '/'
    if (path.length == workArea.length)
        return true;
    return path[workArea.length] == '/';
}

struct RollingAvg {
    import std.datetime : Duration, dur, SysTime, Clock;

    static struct DataPoint {
        SysTime t;
        double v = 0.0;
    }

    private {
        static immutable size_t MinMeasures = 3;
        static immutable size_t MaxMeasures = 100;

        Duration window;
        Duration minStep;
        DataPoint[] measures;
    }

    this(Duration window) @safe pure nothrow {
        this.window = window;
        this.minStep = window / MaxMeasures;
    }

    void put(double v, SysTime now = Clock.currTime) @safe nothrow {
        if (measures.empty) {
            measures ~= DataPoint(now, v);
        } else if ((now - measures[$ - 1].t) >= minStep) {
            measures ~= DataPoint(now, v);
        }

        if (measures.length > MaxMeasures) {
            measures = measures[1 .. $];
        }

        purgeOld(now);
    }

    double avg() @safe pure nothrow const @nogc {
        if (measures.length < MinMeasures)
            return 0;
        auto m0 = measures[0];
        auto m1 = measures[$ - 1];
        auto t = cast(double)((m1.t - m0.t).total!"msecs") / 1000.0;
        if (t < 0.001)
            return 0;
        return (m1.v - m0.v) / t;
    }

    private void purgeOld(const SysTime now) @safe nothrow {
        if (measures.length <= MinMeasures)
            return;

        auto tmp = measures.filter!(a => (now - a.t) < window).array;
        if (tmp.length > MinMeasures) {
            measures = tmp;
        }
    }
}

@("test rolling avg")
unittest {
    import std.conv : to;
    import std.datetime;
    import std.math : abs;

    auto ravg = RollingAvg(10.dur!"seconds");
    assert(ravg.avg == 0.0);

    const t = SysTime(DateTime.init, Duration.zero, UTC());

    // cannot calculate avg on one value
    ravg.put(1.0, t);
    assert(ravg.avg == 0.0);

    // data points that are too close in time are not added
    ravg.put(1.0, t + 1.dur!"msecs");
    assert(ravg.avg == 0.0);
    assert(ravg.measures.length == 1);

    // avg is calculated first when there are a minimum amount
    ravg.put(2.0, t + 1.dur!"seconds");
    assert(ravg.avg == 0.0);
    assert(ravg.measures.length == 2);
    ravg.put(3.0, t + 2.dur!"seconds");
    assert(abs(ravg.avg - 1.0) < 0.00001, ravg.avg.to!string);
    assert(ravg.measures.length == 3);
    ravg.put(4.0, t + 3.dur!"seconds");
    assert(abs(ravg.avg - 1.0) < 0.00001, ravg.avg.to!string);
    assert(ravg.measures.length == 4);

    // old are purged
    foreach (i; 1 .. 11) {
        ravg.put(cast(double) i * 2, t + (3 + i).dur!"seconds");
    }
    assert(abs(ravg.avg - 2.0) < 0.00001, ravg.avg.to!string);
    assert(ravg.measures.length == 10);
}

/// Convert a D tool name to its snake_case model-facing form.
///
/// Identity rule: a name with no uppercase ASCII letters is returned
/// unchanged (already snake_case, kebab-case, empty; MCP external names
/// pass through untouched).
///
/// Conversion (at least one uppercase ASCII letter): letters are
/// lowercased and '_' is inserted
/// - at every [lower|digit] -> [upper] transition (fooBar -> foo_bar),
/// - inside a run of >= 2 uppercase letters, before the last uppercase
///   when it is followed by a lowercase (listRAGDatabases ->
///   list_rag_databases),
/// - but never before a trailing uppercase run (md5RAG -> md5_rag),
/// - and never at [upper] -> [digit] (HTTP2 -> http2).
/// Existing '_' and '$' pass through verbatim (not collapsed). Boundaries
/// are defined over ASCII classes only; non-ASCII bytes pass through
/// verbatim (no boundary is inserted around them). The output charset has
/// no uppercase, so a second application hits the identity rule
/// (idempotent by construction).
string toSnakeCase(string name) @safe pure nothrow {
    import std.ascii : isUpper, isLower, isDigit, toLower;

    bool hasUpper;
    foreach (c; name) {
        if (c.isUpper) {
            hasUpper = true;
            break;
        }
    }
    if (!hasUpper)
        return name;

    string rval;
    rval.reserve(name.length);
    foreach (size_t i, char c; name) {
        if (!c.isUpper) {
            rval ~= c;
            continue;
        }
        if (i > 0) {
            immutable char prev = name[i - 1];
            if (prev.isLower || prev.isDigit)
                rval ~= "_";
            else if (prev.isUpper && i + 1 < name.length && name[i + 1].isLower)
                rval ~= "_";
        }
        rval ~= c.toLower;
    }
    return rval;
}
