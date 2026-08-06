/// CLI entry point for the 'mcp' subcommand -- runs the MCP server over stdio.
///
/// Actor model: the main thread spawns the MCP server as a std.concurrency
/// actor and communicates with it exclusively via messages. There is no
/// shared data, no core.thread and no core.atomic. Termination signals
/// (SIGINT/SIGTERM) are blocked in all threads and consumed by the main
/// thread itself with sigtimedwait, which turns them into shutdown messages.
module llm.app_mcp;

import logger = std.logger;
import std.concurrency : receiveTimeout, send, spawn, thisTid;
import std.datetime : Clock, dur;
import std.exception : collectException;

import core.sys.posix.signal : SIG_BLOCK, SIGINT, SIGTERM, pthread_sigmask,
    sigaddset, sigemptyset, sigset_t, sigtimedwait;
import core.sys.posix.time : timespec;

import llm.app_config : UserConfig;
import llm.mcp_server : McpFailed, McpServerConfig, McpShutdown, McpStarted,
    McpStopped, runMcpServer;

int appMain(UserConfig uconf, UserConfig.Mcp conf) {
    if (conf.listTools) {
        listTools(conf);
    }
    return runServer(conf);
}

private:

// Diagnostic mode: print filtered tool list and exit.
int listTools(UserConfig.Mcp conf) nothrow {
    import std.algorithm : sort;
    import std.array : array, empty;
    import std.json : JSONType;
    import std.stdio : writeln;

    import my.filter : ReFilter;
    import llm.tool_call : descAllFunctions, filterToolDescriptions;

    try {
        auto filter_ = ReFilter(conf.include, conf.exclude);
        auto allTools = descAllFunctions();
        auto filtered = filterToolDescriptions(allTools, filter_);

        if (filtered.array.length == 0) {
            writeln("No tools match the filter (include: ",
                    conf.include.length, ", exclude: ", conf.exclude.length, ")");
        } else {
            writeln("Available tools (", filtered.array.length, "):");
            writeln();
            foreach (tool; filtered.array) {
                auto func = tool["function"];
                auto name = func["name"].str;
                auto desc = func["description"].str;
                writeln("  ", name);
                if (!desc.empty) {
                    writeln("    ", desc);
                }
                // Print parameters if any (sorted for deterministic output).
                if ("parameters" in func && "properties" in func["parameters"]) {
                    auto props = func["parameters"]["properties"];
                    if (props.type == JSONType.object && !props.object.empty) {
                        auto paramNames = props.object.byKey.array;
                        paramNames.sort();
                        foreach (paramName; paramNames) {
                            auto param = props[paramName];
                            string paramType = "string";
                            if ("type" in param)
                                paramType = param["type"].str;
                            string paramDesc = "";
                            if ("description" in param)
                                paramDesc = param["description"].str;
                            string extra = paramDesc.empty ? "" : " - " ~ paramDesc;
                            writeln("      ", paramName, " (", paramType, ")", extra);
                        }
                    }
                }
                writeln();
            }
        }
    } catch (Exception e) {
        logger.error("Error listing tools: ", e.msg).collectException;
        return 1;
    }
    return 0;
}

int runServer(UserConfig.Mcp conf) {
    // Only stdio transport is implemented; require the flag.
    if (!conf.stdio) {
        logger.error("--stdio flag is required (HTTP transport not yet implemented)");
        return 1;
    }

    logger.infof("MCP server starting (stdio, include: %d patterns, exclude: %d patterns)",
            conf.include.length, conf.exclude.length);

    // Block termination signals in this thread BEFORE spawning so that every
    // actor thread inherits the blocked mask and the default signal action
    // can never kill the process. The main thread consumes pending signals
    // with sigtimedwait and converts them into shutdown messages -- no signal
    // handler, no shared state and no extra thread.
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &set, null) != 0) {
        logger.error("Failed to block termination signals");
        return 1;
    }

    // Spawn the MCP server actor. It owns the transport and the MCPServer
    // instance; the main thread only communicates with it via messages.
    auto serverTid = spawn(&runMcpServer, thisTid);
    send(serverTid, McpServerConfig(conf.include.idup, conf.exclude.idup));

    logger.info("MCP server running in actor thread, press Ctrl+C to stop");

    bool running = true;
    bool hadError = false;
    bool shutdownRequested = false;
    immutable ShutdownTimeout = dur!"seconds"(30);
    auto shutdownDeadline = Clock.currTime();
    timespec noWait = timespec(0, 0);
    int sig;

    while (running) {
        if (shutdownRequested && Clock.currTime() > shutdownDeadline) {
            logger.warning("MCP server did not stop in time, exiting");
            hadError = true;
            break;
        }

        // Consume any pending termination signal without blocking.
        sig = sigtimedwait(&set, null, &noWait);
        if (sig == SIGINT || sig == SIGTERM) {
            if (!shutdownRequested) {
                logger.info("Shutdown signal received, requesting actor shutdown");
                shutdownRequested = true;
                shutdownDeadline = Clock.currTime() + ShutdownTimeout;
                send(serverTid, McpShutdown());
            } else {
                logger.warning("Second shutdown signal received, forcing exit");
                hadError = true;
                break;
            }
        }

        receiveTimeout(dur!"msecs"(100), (McpStarted _) {
            logger.info("MCP server actor confirmed startup");
        }, (McpStopped m) {
            logger.info("MCP server actor stopped");
            hadError = m.hadError;
            running = false;
        }, (McpFailed m) { logger.error(m.msg); hadError = true; running = false; });
    }

    logger.info("MCP server stopped");
    return hadError ? 1 : 0;
}
