module llm.memory;

import logger = std.logger;

import core.sync : Condition, Mutex;
import std.algorithm : splitter, canFind, startsWith, map, filter;
import std.datetime : Clock, dur, SysTime;
import std.format : format;
import std.json : parseJSON;
import std.range : retro;
import std.string : strip;
import std.sumtype : match;

import llm.agent : Agent;
import llm.agent_pool : AgentExecutionPool;
import llm.chat : Chat, Message;
import llm.config : LlmConfig;
import llm.metric.monitor : MetricMonitor;
import llm.rag.rag : RAG;
import llm.summary_agent : SummaryAgent;
import llm.types : IAgent, ProcessResult;
import llm.utility : backupMemoryFiles, restoreMemoryFiles, removeMemoryBackup;
import llmfun_tui;

import my.filter : ReFilter;
import my.optional;

/// Orchestrates automatic memory consolidation.
void runMemoryConsolidation(LlmConfig llmConf, RAG rag, MetricMonitor monitor,
        void delegate(string, TuiChatMessageType) sendChatMessage) {
    scope (exit)
        llmConf.clearConsolidationLock();

    logger.infof("Starting memory consolidation (session #%d)", llmConf.sessionCount);

    auto memoryArea = llmConf.memoryArea;

    if (!backupMemoryFiles(memoryArea)) {
        sendChatMessage("[system]: Memory consolidation failed: backup failed.",
                TuiChatMessageType_Assistant);
        return;
    }
    logger.trace("Memory backup created");

    bool successfulConsolidation;
    ProcessResult consolidationResult;
    runConsolidateAgent(llmConf, rag, monitor, sendChatMessage).match!((ProcessResult a) {
        successfulConsolidation = true;
        consolidationResult = a;
    }, (_) {});

    if (consolidationResult.status == ProcessResult.Status.unknownFailure
            || !successfulConsolidation) {
        logger.warning("Memory consolidation agent failed");
        restoreMemoryFiles(memoryArea);
        sendChatMessage("[system]: Memory consolidation failed: agent error. Restored previous memory state.",
                TuiChatMessageType_Assistant);
        return;
    }

    long mergedCount = 0;
    long removedCount = 0;
    bool parsed = false;
    foreach (m; consolidationResult.chat.retro) {
        m.match!((Message msg) {
            foreach (trimmed; msg.content.splitter("\n").map!(a => a.strip)
                .filter!(a => a.startsWith("{") && a.canFind("\"consolidation_result\""))) {
                try {
                    auto json = parseJSON(trimmed);
                    if ("consolidation_result" in json) {
                        auto cr = json["consolidation_result"];
                        if ("merged" in cr) {
                            mergedCount = cr["merged"].array.length;
                        }
                        if ("removed" in cr) {
                            removedCount = cr["removed"].array.length;
                        }
                        parsed = true;
                    }
                } catch (Exception e) {
                    logger.tracef("Failed to parse consolidation JSON: %s", e.msg);
                }
                return;
            }
        }, (_) {});
        if (parsed)
            break;
    }

    removeMemoryBackup(memoryArea);

    string finalAnswer = () {
        if (!parsed) {
            return "[system]: Memory consolidation complete.";
        }
        if (mergedCount == 0 && removedCount == 0) {
            return "[system]: Memory consolidation complete. No changes needed.";
        }
        return format!"[system]: Memory consolidation complete. Merged %s topics, removed %s obsolete topics."(
                mergedCount, removedCount);
    }();
    sendChatMessage(finalAnswer, TuiChatMessageType_FinalAnswer);

    logger.infof("Memory consolidation finished: merged=%s, removed=%s", mergedCount, removedCount);
}

private:

Optional!ProcessResult runConsolidateAgent(LlmConfig llmConf, RAG rag,
        MetricMonitor monitor, void delegate(string, TuiChatMessageType) sendChatMessage) {
    string consolidationPrompt = q{Perform memory consolidation. Follow these steps:

1. Call `getMemoryTopics` to review all memory topics and their summaries.
2. Analyze the topics and identify:
   - Topics that could be merged (similar or overlapping content)
   - Topics that are no longer relevant or obsolete
3. For topics that should be merged:
   - Call `readMemory` on each topic to read full content
   - Combine the content into a single coherent memory
   - Call `writeMemory` to save the consolidated topic
   - Call `removeMemory` on the original topics that were merged
4. For obsolete topics:
   - Call `removeMemory` to delete them
5. At the very end of your response, output a JSON summary line on a single line:
   {"consolidation_result": {"merged": ["topic1", "topic2"], "removed": ["topic3"], "new_topics": ["combined_topic"]}}

If there are no topics to consolidate or remove, output:
{"consolidation_result": {"merged": [], "removed": [], "new_topics": []}}
};

    auto consolidationAgent = new Agent("consolidation", llmConf, monitor, rag, ReFilter.init);
    consolidationAgent.addUserQuery(consolidationPrompt);

    sendChatMessage(format!"[system]: Running memory consolidation (session #%d)..."(
            llmConf.sessionCount), TuiChatMessageType_Assistant);

    auto doneMutex = new Mutex();
    auto doneCond = new Condition(doneMutex);
    bool done = false;
    ProcessResult consolidationResult = ProcessResult.init;

    auto pool = new AgentExecutionPool(1);

    void callback(IAgent a, ProcessResult result) {
        consolidationResult = result;
        doneMutex.lock();
        done = true;
        doneCond.notify();
        doneMutex.unlock();
    }

    try {
        pool.submit(consolidationAgent, &callback);
    } catch (Exception e) {
        logger.errorf("Failed to submit consolidation agent: %s", e.msg);
        sendChatMessage(format!"[system]: Memory consolidation failed: %s. Restored previous memory state."(e.msg),
                TuiChatMessageType_Assistant);
        return none!ProcessResult();
    }

    doneMutex.lock();
    while (!done) {
        doneCond.wait;
    }
    doneMutex.unlock();

    pool.stop();

    return consolidationResult.some;
}
