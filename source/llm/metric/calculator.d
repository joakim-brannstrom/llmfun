module llm.metric.calculator;

import std.algorithm : sort, map;
import std.array : array, appender;
import std.conv : to, text;
import std.format : format, formattedWrite;
import std.math : sqrt;
import std.range : take, put;

import llm.metric.monitor : ToolCallEvent;

/// Info about a tool for report display
struct ToolInfo {
    string name;
    long count; // failures for top failing, successes for top succeeding
    double avgTimeMs; // average response time in milliseconds (for slowest tools)
}

/// Aggregated metrics for the system
struct SystemMetrics {
    double toolSuccessRate = 0.0; // Percentage (0-100)
    double avgResponseTimeMs = 0.0; // Average response time in milliseconds
    double stddevResponseTimeMs = 0.0; // Standard deviation of response times
    ToolInfo[] topFailingTools; // Top 5 tools by failure rate with counts
    ToolInfo[] topSucceedingTools; // Top 5 tools by success rate with counts
    ToolInfo[] slowestTools; // Top 5 tools by average response time
    long totalCalls; // Total number of tool calls
}

/// Calculates metrics from tool call events
struct MetricsCalculator {
    private {
        ToolCallEvent[] events;
        SystemMetrics cachedMetrics;
        bool hasCachedMetrics;
    }

    /// Set events to calculate metrics from
    void setEvents(ToolCallEvent[] events) {
        this.events = events;
        this.hasCachedMetrics = false;
    }

    /// Calculate system metrics from current events
    SystemMetrics calculate(int toolNumber = 5) {
        if (events.length == 0) {
            hasCachedMetrics = false;
            return typeof(return)();
        }

        long successCount = 0;
        double totalTime = 0;
        double[] responseTimes;

        foreach (event; events) {
            if (event.success) {
                successCount++;
            }
            totalTime += event.responseTimeMs;
            responseTimes ~= cast(double) event.responseTimeMs;
        }

        double successRate = cast(double) successCount / events.length * 100;
        double avgTime = totalTime / events.length;
        double stddevTime = sampleStdDev(responseTimes);

        auto toolStats = groupByTool();

        cachedMetrics = SystemMetrics.init;
        cachedMetrics.totalCalls = events.length;
        cachedMetrics.toolSuccessRate = successRate;
        cachedMetrics.avgResponseTimeMs = avgTime;
        cachedMetrics.stddevResponseTimeMs = stddevTime;
        cachedMetrics.topFailingTools = topNByFailureRate(toolStats, toolNumber);
        cachedMetrics.topSucceedingTools = topNBySuccessRate(toolStats, toolNumber);
        cachedMetrics.slowestTools = calculateSlowestTools(toolNumber);

        hasCachedMetrics = true;

        return cachedMetrics;
    }

    /// Generate markdown report from current events
    string generateReport(int toolNumber = 5) {
        import llm.table;

        if (!hasCachedMetrics)
            calculate(toolNumber);

        auto buf = appender!string;

        buf.put("# Tool Call Metrics Report\n\n");
        buf.put("## Summary\n\n");
        buf.put("- **Total Calls**: ");
        buf.put(cachedMetrics.totalCalls.to!string);
        buf.put("\n");
        buf.put("- **Success Rate**: ");
        buf.put(format!("%.1f%%")(cachedMetrics.toolSuccessRate));
        buf.put("\n");
        buf.put("- **Avg Response Time**: ");
        buf.put(format!("%.0f ms")(cachedMetrics.avgResponseTimeMs));
        buf.put("\n");
        buf.put("- **Std Dev Response Time**: ");
        buf.put(format!("%.0f ms")(cachedMetrics.stddevResponseTimeMs));
        buf.put("\n\n");

        if (cachedMetrics.toolSuccessRate < 99.99) {
            buf.put("## Top Failing Tools\n\n");
            auto tbl = Table!3(["Tool", "Failures", "Rank"]);

            foreach (i, info; cachedMetrics.topFailingTools) {
                tbl.put([info.name, info.count.to!string, (i + 1).to!string]);
            }
            formattedWrite(buf, "%s", tbl);
            buf.put("\n");
        }

        {
            buf.put("## Top Succeeding Tools\n\n");
            auto tbl = Table!3(["Tool", "Successes", "Rank"]);
            foreach (i, info; cachedMetrics.topSucceedingTools) {
                tbl.put([info.name, info.count.to!string, (i + 1).to!string]);
            }
            formattedWrite(buf, "%s", tbl);
        }

        if (cachedMetrics.slowestTools.length > 0) {
            buf.put("\n## Slowest Tools\n\n");
            auto tbl = Table!3(["Tool", "Avg Response Time", "Rank"]);

            foreach (i, info; cachedMetrics.slowestTools) {
                tbl.put([
                    info.name, format!"%.0f ms"(info.avgTimeMs), (i + 1).to!string
                ]);
            }
            formattedWrite(buf, "%s", tbl);
            buf.put("\n");
        }

        return buf[];
    }

private:

    struct ToolStats {
        string name;
        long total;
        long failures;

        double failureRate() const {
            return total > 0 ? cast(double) failures / total : 0;
        }

        double successRate() const {
            return total > 0 ? cast(double)(total - failures) / total : 0;
        }
    }

    ToolStats[] groupByTool() {
        long[string] toolTotal;
        long[string] toolFailures;

        foreach (event; events) {
            toolTotal.update(event.toolName, () => 1, (ref long a) { a++; });
            if (!event.success) {
                toolFailures.update(event.toolName, () => 1, (ref long a) { a++; });
            }
        }

        ToolStats[] result;
        foreach (name; toolTotal.keys) {
            long failures = toolFailures.get(name, 0);
            result ~= ToolStats(name: name, total: toolTotal[name], failures: failures);
        }
        return result;
    }

    ToolInfo[] topNByFailureRate(ToolStats[] stats, long n) {
        return stats.sort!((a, b) => a.failureRate > b.failureRate).take(n)
            .map!(a => ToolInfo(name: a.name, count: a.failures)).array;
    }

    ToolInfo[] topNBySuccessRate(ToolStats[] stats, long n) {
        return stats.sort!((a, b) => a.successRate > b.successRate).take(n)
            .map!(a => ToolInfo(name: a.name, count: a.total - a.failures)).array;
    }

    ToolInfo[] calculateSlowestTools(long n) {
        struct ToolTime {
            string name;
            double totalTime;
            long count;

            double avgTime() const {
                return count > 0 ? totalTime / count : 0;
            }
        }

        double[string] toolTotalTime;
        long[string] toolCount;

        foreach (event; events) {
            toolTotalTime.update(event.toolName, () => 0.0, (ref double a) {
                a += event.responseTimeMs;
            });
            toolCount.update(event.toolName, () => 0L, (ref long a) { a++; });
        }

        ToolTime[] toolTimes;
        foreach (name; toolTotalTime.keys) {
            toolTimes ~= ToolTime(name: name, totalTime: toolTotalTime[name],
                    count: toolCount[name]);
        }

        return toolTimes.sort!((a, b) => a.avgTime() > b.avgTime()).take(n)
            .map!(a => ToolInfo(name: a.name, count: a.count, avgTimeMs: a.avgTime())).array;
    }
}

private:

double sampleStdDev(double[] values) {
    if (values.length < 2)
        return 0;
    double mean = 0;
    foreach (v; values)
        mean += v;
    mean /= values.length;

    double sumSquaredDiff = 0;
    foreach (v; values) {
        double diff = v - mean;
        sumSquaredDiff += diff * diff;
    }
    return sqrt(sumSquaredDiff / (values.length - 1));
}
