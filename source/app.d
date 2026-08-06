module app;

import logger = std.logger;
import std.format : format;

import argparse : CLI, matchCmd;
import colorlog;

import llm.app_config : UserConfig;

int main(string[] args) {
    UserConfig cli;
    if (!CLI!UserConfig.parseArgs(cli, args[1 .. $]))
        return 1;
    confLogger(cli.verbosity);
    logger.trace(cli);

    import llm.utility : catchSignalSIGPIPE;

    catchSignalSIGPIPE;

    return matchCmd!((a) => appMain(cli, a))(cli.cmd);
}

int appMain(UserConfig uconf, UserConfig.AgentChatConfig conf) {
    static import llm.app_agent;

    return llm.app_agent.appMain(uconf, conf);
}

int appMain(UserConfig uconf, UserConfig.Rag conf) {
    static import llm.app_rag;

    return llm.app_rag.appMain(uconf, conf);
}

int appMain(UserConfig uconf, UserConfig.PrintToolMetricsConfig conf) {
    static import llm.app_tool_metrics;

    return llm.app_tool_metrics.appMain(uconf, conf);
}

int appMain(UserConfig uconf, UserConfig.Mcp conf) {
    static import llm.app_mcp;

    return llm.app_mcp.appMain(uconf, conf);
}
