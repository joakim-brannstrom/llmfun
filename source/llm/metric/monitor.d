module llm.metric.monitor;

import logger = std.logger;
import std.algorithm : filter, splitter;
import std.array : empty;
import std.datetime : Clock;
import std.file : exists, readText;
import std.json : JSONValue, parseJSON;
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
            File(dataFile.toString, "a").writeln(json);
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
                    events ~= jsonToEvent(parseJSON(line));
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

private:

long currentTimestamp() {
    import std.datetime : SysTime, DateTime;

    return (Clock.currTime - SysTime(DateTime.init)).total!"msecs";
}

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
