/// Handles the 'tool_metrics' subcommand: tool call metrics monitoring and reporting.
module llm.app_tool_metrics;

import logger = std.logger;
import core.thread : Thread;
import core.time : dur;

import std.algorithm : countUntil, map, joiner;
import std.array : empty;
import std.conv : text;
import std.json : parseJSON;
import std.stdio : writeln, writefln;
import std.conv : to;

import colorlog : color, Color;

import llm.app_config : UserConfig, userToLlmConfig;

int appMain(UserConfig uconf, UserConfig.PrintToolMetricsConfig conf) {
    import llm.metric.calculator;
    import llm.metric.monitor;

    if (conf.follow) {
        ToolCallEvent lastEvent;
        while (true) {
            try {
                scope monitor = new MetricMonitor(conf.data);
                auto events = monitor.getRecentEvents(50);
                auto idx = events.countUntil!(a => a == lastEvent);
                events = idx < 0 ? events : events[idx + 1 .. $];
                foreach (e; events) {
                    auto args = e.arguments.object.byKeyValue.map!(
                            a => i"$(a.key.color(Color.yellow)):$(a.value.toString)".text).joiner(
                            ", ");
                    writefln("%s - agent:%s time:%s tool:%s(%s) -> %s",
                            e.timestamp.to!string.color(e.success ? Color.green
                                : Color.red), e.agentName, e.responseTimeMs == 0
                            ? "0 ms" : e.responseTimeMs
                                .dur!"msecs"
                                .to!string, e.toolName, args, e.result);

                }
                if (!events.empty) {
                    lastEvent = events[$ - 1];
                }
            } catch (Exception e) {
                logger.trace(e.msg);
            }
            Thread.sleep(3.dur!"seconds");
        }
    } else {
        try {
            scope monitor = new MetricMonitor(conf.data);
            scope calculator = new MetricsCalculator();
            calculator.setEvents(monitor.getRecentEvents(10000));
            writeln(calculator.generateReport(conf.number));
        } catch (Exception e) {
            writeln("Error: ", e.msg);
            return 1;
        }
    }

    return 0;
}
