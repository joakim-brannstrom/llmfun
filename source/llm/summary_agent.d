module llm.summary_agent;

import core.thread : Thread;
import core.time : dur;
import logger = std.logger;
import std.algorithm : map, filter, canFind, startsWith, sort, min, sum, among;
import std.array : array, appender, empty;
import std.conv : to;
import std.file : readText;
import std.format : format, formattedWrite;
import std.json : JSONValue, parseJSON, JSONType;
import std.range : enumerate, iota;
import std.string : strip, replace, toUpper, toLower;
import std.sumtype : SumType, match;
import std.typecons : Tuple, tuple;

import my.path;
import my.set;

import llm.chat;
import llm.config : SummaryModelConfig, toRequestConfig, getEnvApiKey;
import llm.utility : ApproxTokenSize, summarizeToolCalls;
import llm.query;

struct SummaryAgent {
    private {
        LlmRequester rqSummary;
        string summaryPrompt;
        long contextSize;
        immutable AnswerSize = 8192;
        immutable MaxValidationIterations = 3;
        immutable MinSummaryLength = 20;
        immutable MaxToolResponse = 100;
        immutable KeepLast = 5;
        immutable TokenBudget = 4096L;
    }

    string formatMessagesToText(Chat.MessageT[] messages) {
        auto buf = appender!string();
        foreach (i, msg; messages) {
            auto content = msg.match!((Message m) => format!("%s: %s")(m.role.to!string,
                    m.content), (ToolMessage m) => format!("%s: tool_calls=[%s]")(m.role.to!string,
                    summarizeToolCalls(m.role, m.toolCalls)), (ToolResponse m) => format!("%s: %s")(m.role.to!string,
                    m.content.length < MaxToolResponse ? m.content : m.content[0 .. MaxToolResponse]),
                    (VisionMessage m) => "user: " ~ m.content ~ " [image]");
            buf.put(content);
            if (i < messages.length - 1)
                buf.put('\n');
        }
        return buf[];
    }

    this(SummaryModelConfig confSummary) {
        this.rqSummary = LlmRequester(confSummary.toRequestConfig);

        auto slot = LlmSlotRequester(confSummary.toRequestConfig);
        this.contextSize = slot.request(min(confSummary.contextSize, confSummary.contextChunkSize));
    }

    void setSystemPrompt(string x) {
        this.summaryPrompt = x;
    }

    struct CompressResult {
        bool compressed;
        size_t originalLength;
        size_t newLength;
        size_t keptXCount;
        long keptXTokens;
        size_t summarizedCount;
        long newContextSize;
        size_t chunkCount;
        size_t successfulChunks;
        size_t failedChunks;
        size_t purgedCount;
    }

    /// Result of requestSummary containing summaries and chunk statistics
    struct RequestSummaryResult {
        Tuple!(string, size_t, size_t)[] summaries;
        size_t chunkCount;
        size_t successfulChunks;
        size_t failedChunks;
    }

    /// Callback type for progress reporting during compression.
    /// @param currentChunk  1-based index of current chunk being processed
    /// @param totalChunks   total number of chunks to process
    /// @param status        human-readable status message
    alias ProgressCallback = void delegate(size_t currentChunk, size_t totalChunks, string status);

    /// Filter out ToolMessage and ToolResponse entries matching an exclusion list.
    /// Returns a new array with excluded tool calls removed.
    /// Tracks the number of messages purged in purgedCount.
    private Tuple!(Chat.MessageT[], size_t) purgeTools(Chat.MessageT[] history,
            string[] excludedTools_) {
        if (excludedTools_.empty)
            return typeof(return)(history, 0);

        Chat.MessageT[] result;
        size_t purgedCount = 0;
        Set!string removedCallIds;
        Set!string excludedTools = excludedTools_.toSet;

        // System prompt (index 0) is always preserved
        result ~= history[0];

        foreach (msg; history[1 .. $]) {
            msg.match!((Message m) {
                // Regular messages always kept
                result ~= msg;
            }, (ToolMessage m) {
                foreach (call; m.getFunctions.filter!(a => excludedTools.contains(a.name))) {
                    removedCallIds.add(call.callId);
                }
                foreach (toolName; excludedTools_) {
                    m.removeTool(toolName);
                }

                if (m.getFunctions.empty) {
                    purgedCount++;
                } else {
                    result ~= Chat.MessageT(m);
                }
            }, (ToolResponse m) {
                // Skip only if the specific call ID was removed (not just by name)
                if (removedCallIds.contains(m.toolCallId)) {
                    purgedCount++;
                } else {
                    result ~= msg;
                }
            }, (VisionMessage m) {
                // Vision messages always kept
                result ~= msg;
            });
        }

        return typeof(return)(result, purgedCount);
    }

    /// Compress the chat history using a token-budget approach.
    /// Keeps last KeepLast messages (Y), fills X from newest backwards up to TokenBudget,
    /// and summarizes remaining messages.
    /// Returns result with details about the compression.
    CompressResult compress(ref Chat chat, ProgressCallback callback = null,
            string[] excludedTools_ = null) {
        size_t purgedCount = 0;

        // Purge excluded tools before compression
        auto allMessages = chat.getMessages;
        const historyLen = allMessages.length;
        if (!excludedTools_.empty) {
            auto history = chat.getMessages;
            auto result = purgeTools(history, excludedTools_);
            if (result[1] != 0) {
                purgedCount = result[1];
                allMessages = result[0];
            }
        }

        size_t chunkCount;
        size_t successfulChunks;
        size_t failedChunks;

        if (allMessages.length <= 1 + KeepLast) {
            logger.tracef("Chat too short to compress (length: %s, need at least %s)",
                    allMessages.length, 1 + KeepLast);
            return CompressResult(compressed: false, purgedCount: purgedCount);
        }

        // Step 1: Identify Y — last KeepLast messages
        auto Y = allMessages[$ - KeepLast .. $];

        // Enforce token budget on verbatim Y messages
        for (size_t i = 0; i < Y.length; i++) {
            auto msgTokens = estimateTokens(Y[i]);
            if (msgTokens > TokenBudget) {
                logger.warningf("Verbatim message %s exceeds token budget (%s > %s), summarizing",
                        i, msgTokens, TokenBudget);
                Y[i] = summarizeSingleMessage(Y[i]);
            }
        }

        // Step 2: Identify candidate pool — messages [1 .. $ - KeepLast] (skip system prompt)
        auto candidates = allMessages[1 .. $ - KeepLast];
        if (candidates.empty) {
            logger.trace("No candidate messages to compress");
            return CompressResult(compressed: false, purgedCount: purgedCount);
        }

        // Step 3: Build X — iterate candidates newest to oldest, prepend until TokenBudget
        Chat.MessageT[] X;
        long tokensUsed = 0L;
        size_t xCount = 0;
        foreach (i; 0 .. candidates.length) {
            auto idx = candidates.length - 1 - i; // newest first
            auto msg = candidates[idx];

            // Apply summarizeToolCalls to tool messages
            auto processed = summarizeToolCallsIfNeeded(msg);
            auto msgTokens = estimateTokens(processed);

            if (tokensUsed + msgTokens <= TokenBudget) {
                X = [processed] ~ X;
                tokensUsed += msgTokens;
                xCount++;
            } else {
                break;
            }
        }

        // Remaining = candidates that didn't fit into X
        auto remaining = candidates[0 .. $ - xCount];

        auto keptXCount = X.length;
        auto keptXTokens = tokensUsed;

        // Step 4: Summarize remaining messages
        auto newHistory = [allMessages[0]]; // system prompt

        if (!remaining.empty) {
            auto result = requestSummary(remaining, callback);
            chunkCount = result.chunkCount;
            successfulChunks = result.successfulChunks;
            failedChunks = result.failedChunks;

            logger.infof("Compression chunks: %s total, %s successful, %s failed",
                    chunkCount, successfulChunks, failedChunks);

            if (!result.summaries.empty) {
                newHistory ~= Chat.MessageT(Message(Role.assistant,
                        mergeSummary(result.summaries)));
            } else {
                logger.warning("All chunks failed to produce summaries");
            }
        }

        // Step 5: Build new history: [system_prompt, summaries..., X..., Y...]
        newHistory ~= X;
        newHistory ~= Y;

        chat.setHistory(newHistory);

        logger.tracef("Compressed chat: %s -> %s messages (X+Y kept: %s, summarized: %s)",
                historyLen, newHistory.length, X.length + Y.length, remaining.length);

        return CompressResult(compressed: true, originalLength: historyLen,
                newLength: newHistory.length, keptXCount: keptXCount,
                keptXTokens: keptXTokens, summarizedCount: remaining.length,
                newContextSize: chat.approxContextSize, chunkCount: chunkCount, successfulChunks: successfulChunks,
                failedChunks: failedChunks, purgedCount: purgedCount);
    }

    /// Estimate token count of a message (role + content)
    long estimateTokens(Chat.MessageT msg) {
        auto text = msg.match!((Message m) => m.role.to!string ~ ": " ~ m.content,
                (ToolMessage m) => summarizeToolCalls(m.role, m.toolCalls),
                (ToolResponse m) => m.role.to!string ~ ": " ~ m.content,
                (VisionMessage m) => "user: " ~ m.content ~ " [image]");
        return cast(long) text.length / ApproxTokenSize;
    }

    /// Apply summarizeToolCalls to tool messages to reduce size
    Chat.MessageT summarizeToolCallsIfNeeded(Chat.MessageT msg) {
        auto r = msg.match!((Message m) => Chat.MessageT(m),
                (ToolMessage m) => Chat.MessageT(m), (ToolResponse m) => Chat.MessageT(m),
                (VisionMessage m) => Chat.MessageT(m));
        return r;
    }

    /// Summarize a single oversized message to fit within TokenBudget.
    /// Returns the original message if summarization fails.
    Chat.MessageT summarizeSingleMessage(Chat.MessageT msg) {
        // Extract content from the message
        string content;
        Role role;
        msg.match!((Message m) { content = m.content; role = m.role; }, (ToolMessage m) {
            content = summarizeToolCalls(m.role, m.toolCalls);
            role = m.role;
        }, (ToolResponse m) { content = m.content; role = m.role; }, (VisionMessage m) {
            content = m.content;
            role = Role.user;
        });

        // Build a minimal chat with system prompt and the message to summarize
        Chat summaryChat;
        summaryChat.add(Message(Role.system, "You are a helpful assistant that summarizes text. "
                ~ "Condense the following message while preserving all key information. "
                ~ "Keep the summary concise but complete."));
        summaryChat.add(Message(role, content));

        auto response = request(rqSummary, summaryChat);
        if (response.gotResponse && !response.response.empty) {
            logger.tracef("Summarized oversized message: %s -> %s chars",
                    content.length, response.response.length);
            return Chat.MessageT(Message(role, response.response));
        }

        if (content.length / ApproxTokenSize > TokenBudget) {
            content = content[0 .. TokenBudget];
            logger.warningf("Failed to summarize single message, returning trunkated (%s chars)",
                    content.length);
            return Chat.MessageT(Message(role, content));
        }
        logger.warningf("Failed to summarize single message, returning original (%s chars)",
                content.length);
        return msg;
    }

    Tuple!(string, size_t, size_t)[] buildConversationText(Chat.MessageT[] messages, long maxTokens) {
        typeof(return) rval;
        auto buf = appender!(char[])();
        size_t start;
        size_t curr;
        foreach (msg; messages) {
            auto m = msg.match!((Message m) { return m.toJson.toString; }, (ToolMessage m) {
                return m.toJson.toString;
            }, (ToolResponse m) { return m.toJson.toString; }, (VisionMessage m) {
                return m.toJson.toString;
            });
            if (((m.length + buf.length) / ApproxTokenSize) > maxTokens) {
                rval ~= tuple(buf[].idup, start, curr);
                start = curr;
                buf.clear;
            }
            if (m.length / ApproxTokenSize < maxTokens)
                formattedWrite(buf, "%s\n", m);
            curr++;
        }
        if (!buf.empty)
            rval ~= tuple(buf[].idup, start, curr);
        return rval;
    }

    /// Send summary request to LLM over HTTP
    /// Returns summaries and chunk statistics via a result struct.
    RequestSummaryResult requestSummary(Chat.MessageT[] messages, ProgressCallback callback = null) {
        if (summaryPrompt.empty) {
            logger.warning("No system prompt set");
            return RequestSummaryResult.init;
        }

        RequestSummaryResult result;
        Tuple!(string, size_t, size_t)[] summaries;
        size_t chunkCount;
        size_t successfulChunks;
        size_t failedChunks;

        immutable Query = q"(Summarize the conversation below using the JSON schema defined in the system prompt.

Respond ONLY with a single line of valid JSON. Do not add any other text, explanations, or markdown fences. The output must start with `{"summary":` and end with `}`.

The conversation is presented as JSONL - each line is one message in chronological order. Use only the information present; do not invent facts.

Here is an example of the output format you MUST follow:

{"summary": "The user asked about APIs. The assistant explained REST and GraphQL, then wrote a simple GET endpoint in Python using FastAPI. No errors occurred.", "pending_tasks": ["add error handling to the endpoint"], "open_questions": ["should the endpoint use async?"], "failed_attempts": []}

{previous}

Conversation to summarize (JSONL):
```jsonl
{conversation}
)";
        immutable Previous = "Previous summary:
%s

Now summarize the next %s messages, noting any changes, reversals, or continuations.
";

        auto chunks = buildConversationText(messages, contextSize - AnswerSize);
        foreach (conversation; chunks) {
            chunkCount++;

            auto query = Query.replace("{conversation}", conversation[0]);
            if (summaries.empty) {
                query = query.replace("{previous}", "");
            } else {
                auto p = summaries[$ - 1];
                query = query.replace("{previous}", format!Previous(p[0], p[2] - p[1]));
            }

            Chat summaryChat;
            summaryChat.add(Message(Role.system, summaryPrompt));
            summaryChat.add(Message(Role.user, query));

            auto resp = request(rqSummary, summaryChat);
            if (resp.gotResponse) {
                successfulChunks++;
                summaries ~= tuple(resp.response, conversation[1], conversation[2]);
                if (callback) {
                    callback(chunkCount, chunks.length,
                            format("Chunk %s of %s completed successfully",
                                chunkCount, chunks.length));
                }
            } else {
                failedChunks++;
                logger.warningf("Chunk %s produced no summary content, skipping", chunkCount);
                if (callback) {
                    callback(chunkCount, chunks.length,
                            format("Chunk %s of %s failed", chunkCount, chunks.length));
                }
            }
        }

        result.summaries = summaries;
        result.chunkCount = chunkCount;
        result.successfulChunks = successfulChunks;
        result.failedChunks = failedChunks;
        return result;
    }

    /// Build validation prompt for checking summary against original messages
    string buildValidationPrompt(string summaryText, string preservedText, string lastText) {
        immutable ValidationQuery = q"(You are validating a summary against the original conversation messages.

Answer with ONLY "yes" or "no".

Does the summary contradict any of the following messages? (i.e., does the summary say something that is directly opposed to what the messages say?)

Summary to validate:
{summary}

Preserved messages (high importance, must not be contradicted):
{preserved}

Last messages:
{last}

Answer:
)";
        return ValidationQuery.replace("{summary}", summaryText)
            .replace("{preserved}", preservedText).replace("{last}", lastText);
    }

    /// Build fix prompt for correcting a contradictory summary
    string buildFixPrompt(string summaryText, string preservedText, string lastText) {
        immutable FixQuery = q"(The summary contradicts the original messages. Please fix the summary to accurately reflect the conversation.

Original summary (contains errors):
{summary}

Preserved messages (high importance, must not be contradicted):
{preserved}

Last messages:
{last}

Please provide a corrected summary using the same JSON schema as before (summary, pending_tasks, open_questions, failed_attempts).

Respond ONLY with a single line of valid JSON.
)";
        return FixQuery.replace("{summary}", summaryText)
            .replace("{preserved}", preservedText).replace("{last}", lastText);
    }

    /// Ask LLM if summary contradicts preserved/last messages
    /// Returns true if contradiction found (or error), false if clean
    bool hasContradiction(string summaryText, string preservedText, string lastText) {
        auto query = buildValidationPrompt(summaryText, preservedText, lastText);

        Chat validationChat;
        validationChat.add(Message(Role.system, summaryPrompt));
        validationChat.add(Message(Role.user, query));

        auto response = request(rqSummary, validationChat);
        string answerStr = response.response.strip.toUpper;

        if (!response.gotResponse) {
            logger.warning("Validation returned no response, assuming NO contradiction");
            return false; // No contradiction on error
        }
        if (answerStr == "NO") {
            return false; // No contradiction
        }
        if (answerStr == "YES") {
            logger.warning("Contradiction detected in summary");
            return true;
        }
        logger.warningf("Unexpected validation answer: '%s', assuming NO", answerStr);
        return false;
    }

    /// Ask LLM to fix a contradictory summary
    /// Returns fixed summary or empty string on failure
    string fixSummaryWithLLM(string brokenSummary, string preservedText, string lastText) {
        auto query = buildFixPrompt(brokenSummary, preservedText, lastText);

        Chat fixChat;
        fixChat.add(Message(Role.system, summaryPrompt));
        fixChat.add(Message(Role.user, query));

        auto fixResponse = request(rqSummary, fixChat);
        if (fixResponse.gotResponse && !fixResponse.response.empty) {
            return fixResponse.response;
        }
        return null;
    }

    /// Validate summary against preserved + last messages using LLM
    /// If contradiction detected, prompt model to fix the summary
    /// Iterates up to MaxValidationIterations times
    bool validateAndFixSummary(ref Tuple!(string, size_t, size_t)[] summary,
            Chat.MessageT[] preserved, Chat.MessageT[] last, ref Chat chat) {
        if (summary.empty || summary.map!(a => a[0].length).sum < MinSummaryLength)
            return false;

        string mergedSummary = mergeSummary(summary);
        auto preservedText = formatMessagesToText(preserved);
        auto lastText = formatMessagesToText(last);

        for (size_t iteration = 0; iteration < MaxValidationIterations; iteration++) {
            if (!hasContradiction(mergedSummary, preservedText, lastText)) {
                logger.tracef("Validation passed (iteration %s): no contradiction found",
                        iteration + 1);
                return true;
            }

            logger.warningf("Validation failed (iteration %s): contradiction detected, fixing summary",
                    iteration + 1);
            auto fixed = fixSummaryWithLLM(mergedSummary, preservedText, lastText);
            if (fixed.empty) {
                logger.warning("Failed to fix summary, keeping original");
                return false;
            }

            mergedSummary = fixed;
            summary.length = 0;
            summary ~= tuple(fixed, 0UL, 0UL);
            logger.tracef("Summary fixed in iteration %s", iteration + 1);
        }

        logger.warning("Max validation iterations reached (%s), accepting summary with possible contradictions",
                MaxValidationIterations);
        return true;
    }
}

string mergeSummary(Tuple!(string, size_t, size_t)[] summarize) {
    auto buf = appender!string();
    foreach (a; summarize.enumerate) {
        formattedWrite(buf, "[chunk %s (messages %s-%s) summary]:\n%s\n",
                a.index, a.value[1], a.value[2], a.value[0]);
    }
    return buf[];
}

Tuple!(string, "response", bool, "gotResponse") request(ref LlmRequester rq, ref Chat chat) {
    auto response = rq.request(chat);
    string responseMsg;
    bool gotResponse;
    response.match!((JSONValue j) {
        try {
            foreach (choice; j["choices"].array) {
                responseMsg = choice["message"]["content"].str.strip;
                gotResponse = true;
            }
        } catch (Exception e) {
            logger.warningf("Failed LLM response parse: %s", e);
        }
    }, (LlamaRequestError e) { logger.warningf("Failed LLM request: %s", e); });

    return typeof(return)(responseMsg, gotResponse);
}
