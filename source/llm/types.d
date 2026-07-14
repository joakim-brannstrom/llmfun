module llm.types;

import std.json : JSONValue;

import llm.chat : Chat;
import llm.summary_agent : SummaryAgent;
import llm.tool_call.pipeline : PipelineControlContext;

interface IAgent {
    string id();

    ProcessResult runToCompletion(void delegate(ProcessResult) step = null,
            SummaryAgent.ProgressCallback compressCallback = null, bool delegate() interrupt = null);
}

interface IBasicAgent : IAgent {
    /// Feed a user query/input to the agent.
    void addUserQuery(string query);

    /// Set the pipeline control context for tool call coordination.
    void setPipelineContext(PipelineControlContext ctx);
}

struct ServerStat {
    long context;
    double predictedPerSecond = 0;
    double promptPerSecond = 0;

    string toString() @safe const {
        import std.format : format;

        return format("ServerStat(contextUsed:%s predictedPerSecond:%s promptPerSecond:%s",
                context, predictedPerSecond, promptPerSecond);
    }
}

struct ProcessResult {
    enum Status {
        ok,
        needCompression,
        unknownFailure,
        networkFailure,
        needMoreThinking
    }

    Status status;
    Chat.MessageT[] chat;
    bool hasToolCall;

    ServerStat stat;

    long totalTokens() @safe pure nothrow const {
        return stat.context;
    }
}
