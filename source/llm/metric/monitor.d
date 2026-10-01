module llm.metric.monitor;

import logger = std.logger;
import std.algorithm : filter, splitter;
import std.array : empty;
import std.datetime : Clock;
import std.file : exists, readText;
import std.json : JSONValue, parseJSON, JSONOptions;
import std.stdio : File;

import my.path;

struct ToolCallEvent {
    string agentName;
    string toolName;
    JSONValue arguments;
    long timestamp;
    bool success;
    string result;
    long responseTimeMs;
}

/// Bounded event buffer with JSONL persistence
class MetricMonitor {
    private {
        ToolCallEvent[] events; // In-memory buffer (max 10,000)
        Path dataFile; // JSONL file for persistence
        immutable MaxEvents = 10_000;
        immutable MaxResultLength = 200;
        immutable MaxLogFileSize = 20 * 1024 * 1024;
    }

    this(Path dataFile) {
        this.dataFile = dataFile;
        loadEvents();
    }

    /// Record a tool call event
    void record(string agentName, string toolName, JSONValue args, string result,
            bool success, long responseTimeMs) {
        if (result.length > MaxResultLength) {
            result = result[0 .. MaxResultLength];
        }

        events ~= ToolCallEvent(agentName: agentName, toolName: toolName, arguments: args, timestamp: currentTimestamp(), success: success,
                result: result, responseTimeMs: responseTimeMs);

        if (events.length > MaxEvents) {
            events = events[MaxEvents / 2 .. $];
        }

        saveEvent(events[$ - 1]);
    }

    /// Append a pre-built JSON event (a broker event) to the
    /// JSONL sink. Write-only: the in-memory tool-call buffer and the
    /// feedback engine are not touched — broker events are not
    /// ToolCallEvents.
    void recordEvent(JSONValue ev) @safe {
        try {
            File(dataFile.toString, "a").writeln(ev.toString(JSONOptions.doNotEscapeSlashes));
            trimLogFile;
        } catch (Exception e) {
            logger.tracef("monitor event save failed: %s", e.msg);
        }
    }

    ToolCallEvent[] getRecentEvents(size_t count) {
        if (count >= events.length) {
            return events;
        }
        return events[$ - count .. $];
    }

private:

    void saveEvent(ToolCallEvent event) @safe {
        try {
            auto json = eventToJSON(event);
            File(dataFile.toString, "a").writeln(json.toString(JSONOptions.doNotEscapeSlashes));
            trimLogFile;
        } catch (Exception e) {
            // Never let persistence failures crash the agent
            logger.tracef("monitor save failed: %s", e.msg);
        }
    }

    void loadEvents() @trusted {
        if (!exists(dataFile)) {
            return;
        }

        try {
            foreach (line; File(dataFile).byLine.filter!(a => !a.empty)) {
                try {
                    auto j = parseJSON(line);
                    // Broker events carry a "kind" and no toolName —
                    // skip them: they are not ToolCallEvents, and feedback
                    // does not consume them (they would load as empty-tool
                    // events into the in-memory buffer).
                    if ("kind" in j)
                        continue;
                    events ~= jsonToEvent(j);
                } catch (Exception e) {
                    logger.tracef("monitor load failed for line: %s", e.msg);
                }
            }
        } catch (Exception e) {
            logger.tracef("monitor load failed: %s", e.msg);
        }
    }

    void trimLogFile() @trusted {
        import std.file : getSize, rename;

        if (!dataFile.exists)
            return;
        if (dataFile.getSize < MaxLogFileSize)
            return;

        const tmpFileName = dataFile.toString ~ ".tmp";

        auto tmpFile = File(tmpFileName, "w");
        ulong fileSize;
        foreach (line; File(dataFile).byLine) {
            tmpFile.writeln(line);
            fileSize += line.length;
            if (fileSize > MaxLogFileSize / 2)
                break;
        }

        tmpFile.close;
        rename(tmpFileName, dataFile);
    }
}

public:

/// Milliseconds since the clock's init baseline (Clock.currTime -
/// SysTime(DateTime.init)): the "ts" field of every JSONL event. Public:
/// the broker event emitters stamp their envelopes with it.
long currentTimestamp() @safe {
    import std.datetime : SysTime, DateTime;

    return (Clock.currTime - SysTime(DateTime.init)).total!"msecs";
}

private:
ToolCallEvent jsonToEvent(JSONValue j) @safe {
    import llm.utility : getValue;

    // dfmt off
    return ToolCallEvent(
        agentName: getValue(j, (v) => v["agentName"].str, ""),
        toolName: getValue(j, (v) => v["toolName"].str, ""),
        arguments: getValue(j, (v) => v["arguments"], JSONValue.init),
        timestamp: getValue(j, (v) => v["timestamp"].integer, 0),
        success: getValue(j, (v) => v["success"].boolean, false),
        result: getValue(j, (v) => v["result"].str, ""),
        responseTimeMs: getValue(j, (v) => v["responseTimeMs"].integer, 0));
    // dfmt on
}

JSONValue eventToJSON(ToolCallEvent event) @safe {
    JSONValue j;
    j["agentName"] = event.agentName;
    j["toolName"] = event.toolName;
    j["arguments"] = event.arguments;
    j["timestamp"] = event.timestamp;
    j["success"] = event.success;
    j["result"] = event.result;
    j["responseTimeMs"] = event.responseTimeMs;
    return j;
}

public:

/// Builds a broker JSONL event envelope: "kind" + "ts" +
/// "agent", plus the caller's per-event fields. ts = currentTimestamp().
/// All five broker event kinds go through this.
JSONValue brokerEvent(string kind, string agentName, JSONValue[string] fields) @safe {
    JSONValue ev;
    ev["kind"] = kind;
    ev["ts"] = currentTimestamp();
    ev["agent"] = agentName;
    foreach (k, v; fields)
        ev[k] = v;
    return ev;
}

@("recordEvent appends a broker event line to the JSONL sink")
unittest {
    import std.algorithm : filter;
    import std.array : array;
    import std.datetime : Clock;
    import std.file : mkdirRecurse, rmdirRecurse;
    import std.format : format;
    import std.path : buildPath;
    import std.string : splitLines;

    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/monitor_t10_record_%d_%d", now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto dataFile = buildPath(tmpDir, "monitor.jsonl").Path;

    auto mon = new MetricMonitor(dataFile);
    mon.recordEvent(brokerEvent("broker_prune", "test_agent", [
        "pruned": JSONValue(2L)
    ]));

    const lines = readText(dataFile).splitLines.filter!(a => !a.empty).array;
    assert(lines.length == 1);
    const back = parseJSON(lines[0]);
    assert(back["kind"].str == "broker_prune");
    assert(back["agent"].str == "test_agent");
    assert(back["pruned"].integer == 2);
    assert("toolName" !in back, "broker events carry no toolName");
}

@("loadEvents loads only the ToolCallEvent lines from a mixed JSONL (broker lines are skipped)")
unittest {
    import std.datetime : Clock;
    import std.file : mkdirRecurse, rmdirRecurse, write;
    import std.format : format;
    import std.path : buildPath;

    auto now = Clock.currTime();
    auto tmpDir = format("llmfun_test/monitor_t10_mixed_%d_%d", now.toUnixTime(), now.stdTime);
    mkdirRecurse(tmpDir);
    scope (exit)
        rmdirRecurse(tmpDir);
    auto dataFile = buildPath(tmpDir, "monitor.jsonl").Path;
    write(dataFile, "{\"agentName\":\"a\",\"toolName\":\"writeFile\",\"arguments\":{},"
            ~ "\"timestamp\":1,\"success\":true,\"result\":\"ok\",\"responseTimeMs\":2}\n"
            ~ "{\"kind\":\"broker_prune\",\"ts\":5,\"agent\":\"a\",\"pruned\":2}\n"
            ~ "{\"kind\":\"tools_request\",\"ts\":6,\"agent\":\"a\",\"toolsCount\":3,\"schemaTokens\":50}\n");

    auto mon = new MetricMonitor(dataFile);
    auto events = mon.getRecentEvents(10);
    assert(events.length == 1, "only the ToolCallEvent line loads");
    assert(events[0].toolName == "writeFile");
    assert(events[0].agentName == "a");
    assert(events[0].success);
}
