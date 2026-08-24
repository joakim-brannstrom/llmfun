module llm.summary_agent;

import core.thread : Thread;
import core.time : dur;
import logger = std.logger;
import std.algorithm : map, filter, canFind, startsWith, endsWith, sort, min,
    sum, among, splitter;
import std.array : array, appender, empty;
import std.conv : to, text;
import std.datetime : Clock, SysTime;
import std.file : readText;
import std.format : format, formattedWrite;
import std.json : JSONValue, parseJSON, JSONType, JSONOptions;
import std.range : enumerate, iota;
import std.string : strip, replace, toUpper, toLower;
import std.sumtype : SumType, match;
import std.typecons : Tuple, tuple;

import my.path;
import my.set;

import llm.chat;
import llm.common.config : ApproxTokenSize;
import llm.config : SummaryModelConfig, toRequestConfig, getEnvApiKey;
import llm.query : LlmRequester, toJson, LlamaRequestError;
import llm.utility : summarizeToolCalls, summarizeToolResponse;

alias SummaryChunkT = Tuple!(string, "summary", size_t, "startMessageIndex",
        size_t, "endMessageIndex");

// Callback type for merging multiple chunk summaries into a single string.
alias MergeCallback = string delegate(SummaryChunkT[] summaries);
struct SummaryAgent {
    private {
        LlmRequester rqSummary;
        string summaryPrompt;
        string checkpointSessionId; // owning session for checkpoint events (G1)
        CheckpointListener[] checkpointListeners; // multicast seam (G2)
        long contextSize;
        immutable AnswerSize = 8192;
        immutable MaxValidationIterations = 3;
        immutable MinSummaryLength = 20;
        immutable MaxToolResponse = 100;
        immutable KeepLast = 5;
        immutable TokenBudget = 4096L;
        immutable ToolCallMaxLength = 200;
    }

    string formatMessagesToText(Chat.MessageT[] messages) {
        auto buf = appender!string();
        foreach (i, msg; messages) {
            auto content = msg.match!((Message m) => i"$(m.role): $(m.content)".text,
                    (ToolMessage m) => i"$(m.role): tool_calls=[$(summarizeToolCalls(m.toolCalls,
                        ToolCallMaxLength))]".text,
                    (ToolResponse m) => i"$(m.role): $(m.content.length < MaxToolResponse
                        ? m.content : m.content[0 .. MaxToolResponse])".text,
                    (VisionMessage m) => i"user: $(m.content) [image]".text);
            buf.put(content);
            if (i < messages.length - 1)
                buf.put('\n');
        }
        return buf[];
    }

    this(SummaryModelConfig confSummary) {
        import llm.endpoint : getContextSize;

        this.rqSummary = LlmRequester(confSummary.toRequestConfig);

        this.contextSize = min(confSummary.getContextSize, confSummary.contextChunkSize);
    }

    void setSystemPrompt(string x) {
        this.summaryPrompt = x;
    }

    // Registers a compression checkpoint listener (A6). The seam is
    // multicast (G2): Phase 1 and Phase 2 subscribe independently without
    // one overwriting the other. Listeners must not block — the compressing
    // thread is the UI thread in the common case; a throwing listener is
    // caught and trace-logged, it never breaks compression. The listener
    // list is unsynchronized: registration must complete before compress
    // runs (Phase 0 does not guard concurrent registration, M-2).
    void addCheckpointListener(CheckpointListener listener) {
        if (listener is null)
            return;
        checkpointListeners ~= listener;
    }

    // Sets the owning session id stamped into each CompressionCheckpoint
    // (A6, G1). Call sites that know it (doCompress: activeSession.id) set it
    // before compressing; pool callbacks set their own or leave "" (Phase 1
    // refuses to index events with an empty sessionId).
    void setCheckpointSessionId(string sessionId) {
        checkpointSessionId = sessionId;
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

    // Result of requestSummary containing summaries and chunk statistics
    struct RequestSummaryResult {
        SummaryChunkT[] summaries;
        size_t chunkCount;
        size_t successfulChunks;
        size_t failedChunks;
    }

    // Callback type for progress reporting during compression.
    // @param currentChunk  1-based index of current chunk being processed
    // @param totalChunks   total number of chunks to process
    // @param status        human-readable status message
    alias ProgressCallback = void delegate(size_t currentChunk, size_t totalChunks, string status);

    // A6: structured checkpoint emitted exactly once per compression that
    // actually evicts verbatim content (see compress()). Phase 1 subscribes
    // to index evictedSummarized (+ archive evictedInPlace originals);
    // Phase 2 subscribes for the reasoning trace. Payload arrays are
    // .dup'd copies, safe for asynchronous consumption (L4).
    struct CompressionCheckpoint {
        SysTime timestamp;
        string sessionId; // owning session ("" = session-less chat; Phase 1 refuses to index)
        Chat.MessageT[] evictedSummarized; // the remaining slice summarized & removed
        Chat.MessageT[] evictedPurged; // tool messages + matching ToolResponses removed by purgeTools
        Chat.MessageT[] evictedInPlace; // originals replaced by summarizeSingleMessage (oversized Y)
        long turnStart; // min turn_id across all evicted arrays
        long turnEnd; // max turn_id across all evicted arrays
        string summaryText; // merged replacement summary ("" if all chunks failed)
        size_t originalLength;
        size_t newLength;
        long newContextSize;
    }

    alias CheckpointListener = void delegate(const CompressionCheckpoint);

    // purgeTools result: the kept history, the pre-removal copies of the
    // messages it discarded (H4), and the purge count.
    private struct PurgeResult {
        Chat.MessageT[] kept;
        Chat.MessageT[] removed; // pre-removal copies incl. matching ToolResponses (H4)
        size_t purgedCount;
    }

    // Filter out ToolMessage and ToolResponse entries matching an exclusion list.
    // Returns the kept history, the purged messages (pre-removal copies,
    // including the ToolResponses whose call IDs matched — H4), and the
    // number of messages purged in purgedCount.
    private PurgeResult purgeTools(Chat.MessageT[] history, string[] excludedTools_) {
        if (excludedTools_.empty)
            return PurgeResult(history, null, 0);

        Chat.MessageT[] result;
        Chat.MessageT[] removed;
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
                // H4: capture the pre-removal copy BEFORE removeTool rebinds
                // toolCalls, so evictedPurged carries the message as loaded.
                auto preRemoval = Chat.MessageT(m);
                foreach (toolName; excludedTools_) {
                    m.removeTool(toolName);
                }

                if (m.getFunctions.empty) {
                    purgedCount++;
                    removed ~= preRemoval;
                } else {
                    result ~= Chat.MessageT(m);
                }
            }, (ToolResponse m) {
                // Skip only if the specific call ID was removed (not just by name)
                if (removedCallIds.contains(m.toolCallId)) {
                    purgedCount++;
                    removed ~= msg;
                } else {
                    result ~= msg;
                }
            }, (VisionMessage m) {
                // Vision messages always kept
                result ~= msg;
            });
        }

        return PurgeResult(result, removed, purgedCount);
    }

    // Fires one checkpoint to every registered listener (A6, G2). With no
    // listeners, falls back to a structured trace dump — the Phase-0
    // observability baseline. A throwing listener is caught and trace-logged:
    // an indexing failure must never break compression.
    private void fireCheckpoint(const CompressionCheckpoint checkpoint) {
        if (checkpointListeners.empty) {
            logger.tracef("Compression checkpoint: session=%s turns=%s..%s evicted(summarized=%s purged=%s inPlace=%s) %s->%s messages, context=%s, summary=%s chars",
                    checkpoint.sessionId, checkpoint.turnStart,
                    checkpoint.turnEnd, checkpoint.evictedSummarized.length,
                    checkpoint.evictedPurged.length, checkpoint.evictedInPlace.length,
                    checkpoint.originalLength, checkpoint.newLength,
                    checkpoint.newContextSize, checkpoint.summaryText.length);
            return;
        }
        foreach (listener; checkpointListeners) {
            try {
                listener(checkpoint);
            } catch (Throwable t) {
                // Throwable, not Exception: a listener throwing an Error
                // (e.g. a failed assert) must not break compression either —
                // the checkpoint is best-effort observability (M-1).
                logger.tracef("Compression checkpoint listener threw: %s", t.msg);
            }
        }
    }

    // Builds the merged summary Message that replaces the summarized slice
    // (A6, C2): stamped with the slice's turnEnd — NOT the live currentTurnId
    // — so it can sit at index 1 before the kept X/Y tail (whose ids are >=
    // the summarized ids) without breaking I1, and carrying
    // save_data["summary_turn_start"]/["summary_turn_end"] = the summarized
    // turn range for the Phase 3 router.
    private Chat.MessageT buildMergedSummary(string summaryText, long turnStart, long turnEnd) {
        auto m = Message(Role.assistant, userQuery: false, content: summaryText, thinking: null);
        m.turnId = turnEnd;
        m.saveData["summary_turn_start"] = turnStart;
        m.saveData["summary_turn_end"] = turnEnd;
        return Chat.MessageT(m);
    }

    // Compress the chat history using a token-budget approach.
    // Keeps last KeepLast messages (Y), fills X from newest backwards up to TokenBudget,
    // and summarizes remaining messages.
    // Returns result with details about the compression.
    //
    // Pre-existing same-chat cross-thread access (documented, not fixed):
    // app_agent.doCompress can run compress(chat) on the UI thread while
    // runToCompletion mutates the same chat on the worker thread. Phase 0's
    // turn stamping adds one more unsynchronized field (currentTurnId_) to
    // this already-shared history. Serializing chat mutation is explicitly
    // out of Phase-0 scope (M4/R5).
    CompressResult compress(ref Chat chat, ProgressCallback callback = null,
            string[] excludedTools_ = null) {
        size_t purgedCount = 0;
        Chat.MessageT[] evictedPurged; // A6 (H4): tool traffic removed by purgeTools
        Chat.MessageT[] evictedInPlace; // A6 (H3): pre-replacement originals

        // Purge excluded tools before compression
        auto allMessages = chat.getMessages;
        const historyLen = allMessages.length;
        if (!excludedTools_.empty) {
            // purgeTools only reads its input (removeTool mutates its local
            // by-value copies), so allMessages can be reused (M-3).
            auto result = purgeTools(allMessages, excludedTools_);
            if (result.purgedCount != 0) {
                purgedCount = result.purgedCount;
                evictedPurged = result.removed;
                allMessages = result.kept;
            }
        }

        size_t chunkCount;
        size_t successfulChunks;
        size_t failedChunks;

        // TODO: a bug is hidden here. The agent will deadlock if there are too
        // few messages in the history but one or a few of those are so big the
        // context is full.
        // Quirk to document, not fix (A6/H4): when the purge empties the chat
        // below the compression floor, compress returns WITHOUT setHistory —
        // the purge was applied only to the local copy, so nothing was
        // actually evicted and no checkpoint fires (consistent with the
        // "actually evicted" fire rule and the no-behavior-change promise).
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
                // H3: capture the pre-replacement original — the in-place
                // replacement below destroys verbatim content that never
                // reaches evictedSummarized. Captured only on a REAL
                // replacement (I-1): summarizeSingleMessage returns the
                // original unchanged when the LLM fails and the content fits
                // the truncation threshold (the trigger estimates role-
                // prefixed length, so a narrow window triggers without
                // replacing), and the checkpoint must not claim an eviction
                // that never happened. When a purge also ran, the captured
                // copy is the post-purge message (M-5).
                auto original = Y[i];
                bool replaced;
                auto replacement = summarizeSingleMessage(original, replaced);
                if (replaced)
                    evictedInPlace ~= original;
                Y[i] = replacement;
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
        string summaryText; // merged replacement summary ("" when all chunks fail)

        if (!remaining.empty) {
            auto result = requestSummary(remaining, callback);
            chunkCount = result.chunkCount;
            successfulChunks = result.successfulChunks;
            failedChunks = result.failedChunks;

            logger.infof("Compression chunks: %s total, %s successful, %s failed",
                    chunkCount, successfulChunks, failedChunks);

            if (!result.summaries.empty) {
                summaryText = mergeSummary(result.summaries);
                const summaryRange = turnRangeOf(remaining);
                newHistory ~= buildMergedSummary(summaryText,
                        summaryRange.turnStart, summaryRange.turnEnd);
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

        // A6: fire exactly one checkpoint per compression that actually evicts
        // verbatim content — remaining non-empty OR purged messages non-empty
        // OR evictedInPlace non-empty. Fired AFTER setHistory so
        // newContextSize is final, and fired even when all summary chunks
        // failed (the raw messages are still discarded). Payload arrays are
        // .dup'd before firing (L4).
        if (!remaining.empty || !evictedPurged.empty || !evictedInPlace.empty) {
            CompressionCheckpoint checkpoint;
            checkpoint.timestamp = Clock.currTime;
            checkpoint.sessionId = checkpointSessionId;
            checkpoint.evictedSummarized = remaining;
            checkpoint.evictedPurged = evictedPurged;
            checkpoint.evictedInPlace = evictedInPlace;
            // One concatenation per compression (the event path is cold).
            // turnRangeOf counts unstamped (turnId == 0) entries, so a slice
            // containing any of them reports turnStart == 0 — Phase 1
            // consumers must filter turnId != 0 (see llm.chat.turnRangeOf
            // docs, M-4).
            const evictedRange = turnRangeOf(
                    checkpoint.evictedSummarized ~ checkpoint.evictedPurged
                    ~ checkpoint.evictedInPlace);
            checkpoint.turnStart = evictedRange.turnStart;
            checkpoint.turnEnd = evictedRange.turnEnd;
            checkpoint.summaryText = summaryText;
            checkpoint.originalLength = historyLen;
            checkpoint.newLength = newHistory.length;
            checkpoint.newContextSize = chat.approxContextSize;
            fireCheckpoint(checkpoint);
        }

        return CompressResult(compressed: true, originalLength: historyLen,
                newLength: newHistory.length, keptXCount: keptXCount,
                keptXTokens: keptXTokens, summarizedCount: remaining.length,
                newContextSize: chat.approxContextSize, chunkCount: chunkCount, successfulChunks: successfulChunks,
                failedChunks: failedChunks, purgedCount: purgedCount);
    }

    // Estimate token count of a message (role + content)
    long estimateTokens(Chat.MessageT msg) {
        import std.string : join;

        auto text = msg.match!((Message m) => m.role.to!string ~ ": " ~ m.content,
                (ToolMessage m) => summarizeToolCalls(m.toolCalls, ToolCallMaxLength).join('\n'),
                (ToolResponse m) => m.role.to!string ~ ": " ~ m.content,
                (VisionMessage m) => "user: " ~ m.content ~ " [image]");
        return cast(long) text.length / ApproxTokenSize;
    }

    // TODO: it should, for tool messages and response, summarize and convert to a Message.
    // Apply summarizeToolCalls to tool messages to reduce size
    Chat.MessageT summarizeToolCallsIfNeeded(Chat.MessageT msg) {
        auto r = msg.match!((Message m) => Chat.MessageT(m),
                (ToolMessage m) => Chat.MessageT(m), (ToolResponse m) => Chat.MessageT(m),
                (VisionMessage m) => Chat.MessageT(m));
        return r;
    }

    // Replacement for an in-place summarized message (H3): the original's
    // saveData and turnId are copied onto the replacement Message — the
    // previous bare-Message construction silently dropped both (the latent
    // metadata-loss bug), and the typed turnId keeps I1 intact for kept X/Y
    // entries. The pre-existing type erasure (ToolMessage/ToolResponse/
    // VisionMessage -> Message) remains, documented as a Phase-0
    // no-behavior-change boundary.
    private Chat.MessageT replacementFor(Chat.MessageT original, Role role, string content) {
        JSONValue saveData;
        original.match!((Message m) { saveData = m.saveData; }, (ToolMessage m) {
            saveData = m.saveData;
        }, (ToolResponse m) { saveData = m.saveData; }, (VisionMessage m) {
            saveData = JSONValue.init;
        });
        // L4: copy the entries into a fresh JSONValue so the replacement does
        // NOT alias the checkpoint payload's saveData AA (JSONValue copies
        // share the underlying object — the same care as chat.d's
        // saveDataWithTurnId). The live replacement would otherwise share one
        // AA with the evictedInPlace "original" already handed to listeners.
        if (saveData.type == JSONType.object) {
            JSONValue fresh;
            foreach (key, val; saveData.object) {
                fresh[key] = val;
            }
            saveData = fresh;
        }
        auto replacement = Message(role, userQuery: false, content: content,
                thinking: null, saveData: saveData);
        replacement.turnId = turnIdOf(original);
        return Chat.MessageT(replacement);
    }

    // Summarize a single oversized message to fit within TokenBudget.
    // Returns the original message if summarization fails.
    // out replaced: true when the returned message is a NEW message (the
    // original was discarded — an actual eviction), false when the original
    // is returned unchanged (no eviction happened; I-1).
    Chat.MessageT summarizeSingleMessage(Chat.MessageT msg, out bool replaced) {
        import std.string : join;

        replaced = false;

        // Extract content from the message
        string content;
        Role role;
        msg.match!((Message m) { content = m.content; role = m.role; }, (ToolMessage m) {
            content = summarizeToolCalls(m.toolCalls, ToolCallMaxLength).join('\n');
            role = m.role;
        }, (ToolResponse m) {
            content = summarizeToolResponse(m, 8196);
            role = m.role;
        }, (VisionMessage m) { content = m.content; role = Role.user; });

        // Build a minimal chat with system prompt and the message to summarize
        Chat summaryChat;
        summaryChat.add(Message(Role.system, userQuery: false, content: "You are a helpful assistant that summarizes text. "
                ~ "Condense the following message while preserving all key information. "
                ~ "Keep the summary concise but complete.", thinking: null));
        summaryChat.add(Message(role, userQuery: false, content: content, thinking: null));

        auto response = request(rqSummary, summaryChat);
        if (response.gotResponse && !response.response.empty) {
            logger.tracef("Summarized oversized message: %s -> %s chars",
                    content.length, response.response.length);
            replaced = true;
            return replacementFor(msg, role, response.response);
        }

        if (content.length / ApproxTokenSize > TokenBudget) {
            content = content[0 .. TokenBudget];
            logger.warningf("Failed to summarize single message, returning trunkated (%s chars)",
                    content.length);
            replaced = true;
            return replacementFor(msg, role, content);
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
            auto m = msg.match!((Message m) {
                return m.toJson.toString(JSONOptions.doNotEscapeSlashes);
            }, (ToolMessage m) {
                return m.toJson.toString(JSONOptions.doNotEscapeSlashes);
            }, (ToolResponse m) {
                return m.toJson.toString(JSONOptions.doNotEscapeSlashes);
            }, (VisionMessage m) {
                return m.toJson.toString(JSONOptions.doNotEscapeSlashes);
            });
            if (((m.length + buf[].length) / ApproxTokenSize) > maxTokens) {
                rval ~= tuple(buf[].idup, start, curr);
                start = curr;
                buf.clear;
            }
            if (m.length / ApproxTokenSize < maxTokens)
                formattedWrite(buf, "%s\n", m);
            curr++;
        }
        if (!buf[].empty)
            rval ~= tuple(buf[].idup, start, curr);
        return rval;
    }

    // Send summary request to LLM over HTTP
    // Returns summaries and chunk statistics via a result struct.
    RequestSummaryResult requestSummary(Chat.MessageT[] messages, ProgressCallback callback = null) {
        if (summaryPrompt.empty) {
            logger.warning("No system prompt set");
            return RequestSummaryResult.init;
        }

        RequestSummaryResult result;
        SummaryChunkT[] summaries;
        size_t chunkCount;
        size_t successfulChunks;
        size_t failedChunks;

        immutable Query = q"(Summarize the conversation below as a concise bullet list, as defined in the system prompt.

Respond ONLY using the markdown text. Do not add any other text, explanations, or headings.

The conversation is presented as JSONL - each line is one message in chronological order. Use only the information present; do not invent facts.

Here is an example of the output format you MUST follow:

- The user asked about APIs.
- The assistant explained REST and GraphQL, then wrote a simple GET endpoint in Python using FastAPI. No errors occurred.
- pending task: add error handling to the endpoint
- open questions: should the endpoint use async?

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
            summaryChat.add(Message(Role.system, userQuery: false, content: summaryPrompt,
                    thinking: null));
            summaryChat.add(Message(Role.user, userQuery: false, content: query, thinking: null));

            auto resp = request(rqSummary, summaryChat);
            if (resp.gotResponse) {
                successfulChunks++;
                summaries ~= SummaryChunkT(stripFences(resp.response),
                        conversation[1], conversation[2]);
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

    // Build validation prompt for checking summary against original messages
    string buildValidationPrompt(string summaryText, string preservedText, string lastText) {
        immutable ValidationQuery = q"(You are validating a summary against the original conversation messages.

Answer with ONLY "yes" or "no".

Does the summary contradict any of the following messages? (i.e., does the summary say something that is directly opposed to what the messages say?)

Summary to validate:

---

{summary}

---

Preserved messages (high importance, must not be contradicted):

---

{preserved}

---

Last messages:

---

{last}

---

Answer:
)";
        return ValidationQuery.replace("{summary}", summaryText)
            .replace("{preserved}", preservedText).replace("{last}", lastText);
    }

    // Build fix prompt for correcting a contradictory summary
    string buildFixPrompt(string summaryText, string preservedText, string lastText) {
        immutable FixQuery = q"(The summary contradicts the original messages. Please fix the summary to accurately reflect the conversation.

Original summary (contains errors):

---

{summary}

---

Preserved messages (high importance, must not be contradicted):

---

{preserved}

---

Last messages:

---

{last}

---

Please provide a corrected summary as a concise bullet list, matching the format used before.

Respond ONLY with the bullet list. Do not add any other text, explanations or headings.)";
        return FixQuery.replace("{summary}", summaryText)
            .replace("{preserved}", preservedText).replace("{last}", lastText);
    }

    // Ask LLM if summary contradicts preserved/last messages
    // Returns true if contradiction found (or error), false if clean
    bool hasContradiction(string summaryText, string preservedText, string lastText) {
        auto query = buildValidationPrompt(summaryText, preservedText, lastText);

        Chat validationChat;
        validationChat.add(Message(Role.system, userQuery: false, content: summaryPrompt,
                thinking: null));
        validationChat.add(Message(Role.user, userQuery: false, content: query, thinking: null));

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

    // Ask LLM to fix a contradictory summary
    // Returns fixed summary or empty string on failure
    string fixSummaryWithLLM(string brokenSummary, string preservedText, string lastText) {
        auto query = buildFixPrompt(brokenSummary, preservedText, lastText);

        Chat fixChat;
        fixChat.add(Message(Role.system, userQuery: false, content: summaryPrompt, thinking: null));
        fixChat.add(Message(Role.user, userQuery: false, content: query, thinking: null));

        auto fixResponse = request(rqSummary, fixChat);
        if (fixResponse.gotResponse && !fixResponse.response.empty) {
            return fixResponse.response;
        }
        return null;
    }

    // Validate summary against preserved + last messages using LLM
    // If contradiction detected, prompt model to fix the summary
    // Iterates up to MaxValidationIterations times
    bool validateAndFixSummary(ref SummaryChunkT[] summary,
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
            summary = [SummaryChunkT(fixed, 0UL, 0UL)];
            logger.tracef("Summary fixed in iteration %s", iteration + 1);
        }

        logger.warningf("Max validation iterations reached (%s), accepting summary with possible contradictions",
                MaxValidationIterations);
        return true;
    }

    // Summarize arbitrary text content (e.g., AGENTS.md) into a bullet-list summary.
    // Chunks text to fit within context window, summarizes each chunk, and merges results.
    // Params:
    //  content text to summarize (max 32KB)
    //  maxWords target word count for the final summary (soft target — LLM may produce slightly more or fewer words; default 200)
    //  merge optional merge callback for combining chunk summaries
    // Returns: bullet-list summary string
    string summarizeText(string content, size_t maxWords = 200, MergeCallback merge = null) {
        immutable MaxInputSize = 32 * 1024; // 32KB limit

        if (content.empty) {
            logger.warning("summarizeText: empty content");
            return null;
        }

        if (content.length > MaxInputSize) {
            logger.warningf("summarizeText: content exceeds %s byte limit (%s bytes), truncating",
                    MaxInputSize, content.length);
            content = content[0 .. MaxInputSize];
        }

        if (summaryPrompt.empty) {
            logger.warning("summarizeText: no system prompt set");
            return null;
        }

        // Chunk the text by paragraphs, keeping each chunk within context window
        auto chunks = chunkTextForSummary(content);
        if (chunks.empty) {
            return null;
        }

        SummaryChunkT[] summaries;
        size_t chunkIdx = 0;

        foreach (chunk; chunks) {
            chunkIdx++;

            auto query = "Summarize the following text as a concise bullet list.\n" ~ i"Target approximately $(maxWords) words total. Use bullet points (-) for each key point.\n".text
                ~ "Focus on: capabilities, constraints, commands, tools, and important details.\n" ~ i"\nText to summarize:\n```\n$(
                        chunk)\n```\n".text;

            Chat summaryChat;
            summaryChat.add(Message(Role.system, userQuery: false, content: summaryPrompt,
                    thinking: null));
            summaryChat.add(Message(Role.user, userQuery: false, content: query, thinking: null));

            auto resp = request(rqSummary, summaryChat);
            if (resp.gotResponse && !resp.response.empty) {
                auto summaryText = stripFences(resp.response);
                summaries ~= SummaryChunkT(summaryText, 0UL, 0UL);
                logger.tracef("summarizeText: chunk %s/%s summarized (%s chars)",
                        chunkIdx, chunks.length, summaryText.length);
            } else {
                logger.warningf("summarizeText: chunk %s/%s produced no summary",
                        chunkIdx, chunks.length);
            }
        }

        if (summaries.empty) {
            logger.warning("summarizeText: all chunks failed to produce summaries");
            return null;
        }

        // Merge all chunk summaries into one
        string result = merge is null ? defaultMergeSummary(summaries) : merge(summaries);
        logger.tracef("summarizeText: produced %s-char summary from %s chunks",
                result.length, chunks.length);
        return result;
    }

    // Chunk text into pieces that fit within the context window for summarization.
    // Splits by paragraphs (double newlines) and groups them into chunks.
    // Oversized paragraphs are hard-split at character boundaries.
    private string[] chunkTextForSummary(string content) {
        auto maxChunkSize = (contextSize - AnswerSize) * ApproxTokenSize;

        // Split by paragraphs (double newlines or single newlines)
        auto paragraphs = content.splitter("\n\n").array
            .map!(a => a.strip)
            .filter!(a => !a.empty)
            .array;

        if (paragraphs.empty) {
            // Fallback: split by single newlines
            paragraphs = content.splitter("\n").array
                .map!(a => a.strip)
                .filter!(a => !a.empty)
                .array;
        }

        if (paragraphs.empty) {
            return [content];
        }

        // Group paragraphs into chunks that fit within context window
        string[] chunks;
        auto buf = appender!string();
        foreach (para; paragraphs) {
            // Handle oversized paragraphs by hard-splitting
            if (cast(long) para.length > maxChunkSize) {
                // Flush current chunk first
                if (!buf[].empty) {
                    chunks ~= buf[].idup;
                    buf = appender!string();
                }
                // Hard-split oversized paragraph into fixed-size chunks
                for (size_t offset = 0; offset < para.length; offset += maxChunkSize) {
                    auto end = (offset + maxChunkSize < para.length) ? offset + maxChunkSize
                        : para.length;
                    chunks ~= para[offset .. end].idup;
                }
                continue;
            }

            // Try adding this paragraph to current chunk
            auto testContent = buf[] ~ "\n\n" ~ para;
            auto testTokens = cast(long) testContent.length / ApproxTokenSize;

            if (testTokens > (contextSize - AnswerSize)) {
                // Current chunk is full, save it and start a new one
                if (!buf[].empty) {
                    chunks ~= buf[].idup;
                    buf = appender!string();
                }
            }
            if (buf[].empty) {
                buf.put(para);
            } else {
                buf.put("\n\n");
                buf.put(para);
            }
        }
        if (!buf[].empty) {
            chunks ~= buf[].idup;
        }

        return chunks;
    }
}

string defaultMergeSummary(SummaryChunkT[] summaries) {
    auto buf = appender!string();
    foreach (a; summaries.enumerate) {
        buf.put(i"[chunk $(a.index) (messages $(a.value[1])-$(a.value[2])) summary]:\n$(a.value[0])\n"
                .text);
    }
    return buf[];
}

// Merge an array of chunk summaries into a single string.
// Uses the provided merge callback, or defaults to defaultMergeSummary.
// TODO: remove this function
string mergeSummary(SummaryChunkT[] summaries, MergeCallback merge = null) {
    return merge is null ? defaultMergeSummary(summaries) : merge(summaries);
}

Tuple!(string, "response", bool, "gotResponse") request(ref LlmRequester rq, ref Chat chat) {
    auto response = rq.request(chat);
    string responseMsg;
    bool gotResponse;
    response.toJson.match!((JSONValue j) {
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

/// Strip markdown code fences (``` ... ```) from a string.
private string stripFences(string text) {
    import std.string : join;

    return text.splitter("\n").filter!(a => !a.startsWith("```"))
        .map!(a => a.strip)
        .filter!(a => !a.empty)
        .join("\n");
}

// --- Task 5 tests: compression checkpoint event (A6) ---

version (unittest) {
    // Synthetic SummaryAgent: no system prompt is set, so requestSummary bails
    // out on the empty prompt and no LLM call is made. The constructor only
    // builds the requester config (no network I/O).
    private SummaryAgent makeTestSummaryAgent() {
        SummaryModelConfig cfg;
        cfg.modelName = "test";
        cfg.contextSize = 8192;
        cfg.contextChunkSize = 8192;
        return SummaryAgent(cfg);
    }

    // JSON tool call entry {"function": {"name": name}, "id": callId} — the
    // shape ToolMessage.getFunctions reads.
    private JSONValue makeToolCall(string name, string callId) {
        return JSONValue([
            "function": JSONValue(["name": JSONValue(name)]),
            "id": JSONValue(callId)
        ]);
    }
}

// C2: the merged summary Message is stamped with the summarized slice's
// turnEnd (NOT the live currentTurnId) and carries the turn range in
// save_data for the Phase 3 router. This keeps I1: the summary sits at index
// 1 before the kept X/Y tail, whose ids are >= turnEnd.
unittest {
    auto agent = makeTestSummaryAgent();
    auto summary = agent.buildMergedSummary("- summary line", 3, 7);
    summary.match!((Message m) {
        assert(m.turnId == 7);
        assert(m.saveData["summary_turn_start"].integer == 3);
        assert(m.saveData["summary_turn_end"].integer == 7);
        assert(m.content == "- summary line");
    }, (_) { assert(false, "expected a Message"); });
}

// H3: replacementFor copies the original's saveData and turnId onto the
// replacement Message; the ToolMessage original exercises the type erasure
// (ToolMessage -> Message) documented as a Phase-0 boundary.
unittest {
    auto agent = makeTestSummaryAgent();
    auto tm = ToolMessage("think", JSONValue([makeToolCall("toolB", "call1")]),
            JSONValue.init, JSONValue(["k": JSONValue("v")]));
    tm.turnId = 7;
    auto replacement = agent.replacementFor(Chat.MessageT(tm), Role.assistant, "short");
    replacement.match!((Message m) {
        assert(m.turnId == 7);
        assert(m.saveData["k"].str == "v");
        assert(m.content == "short");
    }, (_) { assert(false, "expected a Message"); });
}

// A6 end-to-end: compressing a chat that evicts turns 1-5 fires exactly one
// checkpoint per listener, with turnStart=1, turnEnd=5, the sessionId the
// call site provided, and the evicted slice matching the discarded messages.
// The oversized turn-5 reply (4500 estimated tokens) pushes every candidate
// out of the X budget, so remaining spans exactly turns 1-5. No summary
// prompt is set, so all summary chunks fail -> the event still fires (the
// raw messages are discarded) and no merged summary message is inserted;
// the kept Y messages retain their original TurnIDs and the history stays
// I1-sorted. Payload arrays are .dup'd copies (L4): mutating history after
// the event does not affect them.
unittest {
    import std.array : replicate;

    immutable OversizedReply = "x".replicate(9000); // 9000 chars / 2 = 4500 tokens
    Chat chat;
    chat.setSystemPrompt("sys");
    foreach (turn; 1 .. 9) { // 8 turns: q1..q8; a5 is oversized; turn 8 has no reply yet
        chat.addUserQuery("q" ~ turn.to!string);
        if (turn < 8) {
            const reply = (turn == 5) ? OversizedReply : "ok";
            chat.add(Message(Role.assistant, userQuery: false, content: reply, thinking: null));
        }
    }
    assert(chat.getMessages.length == 16);

    int events = 0;
    long turnStart = -1;
    long turnEnd = -1;
    string sessionId;
    size_t evictedCount = 0;
    const(Chat.MessageT)[] capturedEvicted;
    auto agent = makeTestSummaryAgent();
    agent.setCheckpointSessionId("sess-1");
    agent.addCheckpointListener((const SummaryAgent.CompressionCheckpoint cp) {
        events++;
        turnStart = cp.turnStart;
        turnEnd = cp.turnEnd;
        sessionId = cp.sessionId;
        evictedCount = cp.evictedSummarized.length;
        capturedEvicted = cp.evictedSummarized.dup;
    });

    auto result = agent.compress(chat);

    assert(result.compressed);
    assert(result.originalLength == 16);
    assert(result.newLength == 6);
    assert(events == 1);
    assert(turnStart == 1);
    assert(turnEnd == 5);
    assert(sessionId == "sess-1");
    assert(evictedCount == 10);
    // evicted slice: first entry is q1 (turn 1), last is the oversized a5
    capturedEvicted[0].match!((Message m) { assert(m.content == "q1"); }, (_) {
        assert(false, "expected q1");
    });
    capturedEvicted[$ - 1].match!((Message m) { assert(m.content.length == 9000); }, (_) {
        assert(false, "expected oversized a5");
    });
    assert(turnRangeOf(capturedEvicted).turnStart == 1);
    assert(turnRangeOf(capturedEvicted).turnEnd == 5);
    // I1: the compressed history stays (turn_id, position)-sorted
    long prev = 0;
    foreach (m; chat.getMessages) {
        const id = turnIdOf(m);
        assert(id >= prev, "history must stay I1-sorted");
        prev = id;
    }
    // kept Y messages retain their original TurnIDs; no merged summary was
    // inserted at index 1 (all chunks failed)
    assert(chat.getMessages.length == 6);
    assert(turnIdOf(chat.getMessages[1]) == 6); // q6
    assert(turnIdOf(chat.getMessages[2]) == 6); // a6
    assert(turnIdOf(chat.getMessages[5]) == 8); // q8
    // L4: the payload arrays are independent copies of history
    chat.getMessages[1] = Chat.MessageT(Message(Role.user, userQuery: false,
            content: "mutated", thinking: null));
    capturedEvicted[0].match!((Message m) {
        assert(m.content == "q1", "payload must be an independent copy");
    }, (_) { assert(false, "expected q1"); });
}

// A6 fire rule: nothing evicted -> no event. (a) A chat below the compression
// floor returns without compressing. (b) A chat whose candidate pool fits X
// entirely is still rewritten by setHistory but discards nothing, so no
// checkpoint fires.
unittest {
    int events = 0;
    auto agent = makeTestSummaryAgent();
    agent.addCheckpointListener((const SummaryAgent.CompressionCheckpoint cp) {
        events++;
    });

    Chat small;
    small.setSystemPrompt("sys");
    small.addUserQuery("q1");
    small.add(Message(Role.assistant, userQuery: false, content: "a1", thinking: null));
    small.addUserQuery("q2");
    small.add(Message(Role.assistant, userQuery: false, content: "a2", thinking: null));
    assert(small.getMessages.length == 5);
    auto result = agent.compress(small);
    assert(!result.compressed);
    assert(events == 0);
    assert(small.getMessages.length == 5); // untouched

    Chat fits;
    fits.setSystemPrompt("sys");
    foreach (turn; 1 .. 5) { // 4 turns, 9 messages: the candidate pool fits X
        fits.addUserQuery("q" ~ turn.to!string);
        fits.add(Message(Role.assistant, userQuery: false, content: "ok", thinking: null));
    }
    assert(fits.getMessages.length == 9);
    auto fitResult = agent.compress(fits);
    assert(fitResult.compressed);
    assert(events == 0); // nothing discarded -> no checkpoint
}

// H4: a purged-tools compression reports the purged messages in
// evictedPurged — pre-removal copies (ToolMessages with their calls intact)
// plus the ToolResponses whose call IDs matched. Kept messages retain their
// original TurnIDs and the partially purged ToolMessage keeps its surviving
// call.
unittest {
    Chat chat;
    chat.setSystemPrompt("sys");
    chat.addUserQuery("q1");
    chat.add(ToolMessage("", JSONValue([
        makeToolCall("toolA", "call1"), makeToolCall("toolB", "call2")
    ]), JSONValue.init, JSONValue.init));
    chat.add(ToolResponse("resp1", "call1", "toolA", true));
    chat.add(Message(Role.assistant, userQuery: false, content: "a1", thinking: null));
    chat.addUserQuery("q2");
    chat.add(ToolMessage("", JSONValue([
        makeToolCall("toolA", "callA1"), makeToolCall("toolA", "callA2")
    ]), JSONValue.init, JSONValue.init));
    chat.add(ToolResponse("resp2", "callA1", "toolA", true));
    chat.add(ToolResponse("resp3", "callA2", "toolA", true));
    chat.add(Message(Role.assistant, userQuery: false, content: "a2", thinking: null));
    chat.addUserQuery("q3");
    chat.add(Message(Role.assistant, userQuery: false, content: "a3", thinking: null));
    assert(chat.getMessages.length == 12);

    int events = 0;
    long turnStart = -1;
    long turnEnd = -1;
    const(Chat.MessageT)[] purged;
    auto agent = makeTestSummaryAgent();
    agent.setCheckpointSessionId("sess-p");
    agent.addCheckpointListener((const SummaryAgent.CompressionCheckpoint cp) {
        events++;
        turnStart = cp.turnStart;
        turnEnd = cp.turnEnd;
        purged = cp.evictedPurged.dup;
    });

    auto result = agent.compress(chat, null, ["toolA"]);

    assert(result.compressed);
    assert(result.purgedCount == 4);
    assert(events == 1);
    assert(turnStart == 1);
    assert(turnEnd == 2);
    assert(purged.length == 4);
    purged[0].match!((ToolResponse m) { assert(m.toolCallId == "call1"); }, (_) {
        assert(false, "expected tr1");
    });
    purged[1].match!((ToolMessage m) {
        assert(m.toolCalls.array.length == 2); // pre-removal copy (H4)
    }, (_) { assert(false, "expected tm2"); });
    purged[2].match!((ToolResponse m) { assert(m.toolCallId == "callA1"); }, (_) {
        assert(false, "expected tr2");
    });
    purged[3].match!((ToolResponse m) { assert(m.toolCallId == "callA2"); }, (_) {
        assert(false, "expected tr3");
    });
    // the kept ToolMessage: original TurnID (1) and only toolB survives
    assert(turnIdOf(chat.getMessages[2]) == 1);
    chat.getMessages[2].match!((ToolMessage m) {
        assert(m.getFunctions.length == 1);
        assert(m.getFunctions[0].name == "toolB");
    }, (_) { assert(false, "expected the kept ToolMessage"); });
    // I1: the compressed history stays sorted
    long prev = 0;
    foreach (m; chat.getMessages) {
        const id = turnIdOf(m);
        assert(id >= prev);
        prev = id;
    }
}

// I-1 regression: the in-place capture fires only on a REAL replacement.
// estimateTokens counts the role prefix, so an assistant Message with content
// in [8182, 8192] chars triggers the Y-loop summarization attempt, but the
// truncation check inside summarizeSingleMessage (content.length / 2 >
// TokenBudget, i.e. content > 8192) does not fire — with the LLM
// unreachable, the original is returned unchanged. Nothing was evicted: no
// checkpoint may fire and the message must stay byte-identical in history.
// (~3.5 s of request retry backoff, same as the H3 test.)
unittest {
    import std.array : replicate;

    // (11 + 8185) / 2 = 4098 > TokenBudget triggers; 8185 / 2 = 4092 <=
    // TokenBudget skips truncation -> original returned unchanged.
    immutable BorderlineReply = "y".replicate(8185);
    Chat chat;
    chat.setSystemPrompt("sys");
    foreach (turn; 1 .. 5) { // 4 turns, 9 messages: floor OK, candidates fit X
        chat.addUserQuery("q" ~ turn.to!string);
        const reply = (turn == 2) ? BorderlineReply : "ok";
        chat.add(Message(Role.assistant, userQuery: false, content: reply, thinking: null));
    }
    assert(chat.getMessages.length == 9);

    int events = 0;
    auto agent = makeTestSummaryAgent();
    agent.addCheckpointListener((const SummaryAgent.CompressionCheckpoint cp) {
        events++;
    });

    auto result = agent.compress(chat);

    assert(result.compressed); // history rewritten...
    assert(events == 0); // ...but nothing was discarded -> no checkpoint (I-1)
    // the borderline message is still verbatim in history
    bool foundVerbatim = false;
    foreach (m; chat.getMessages) {
        m.match!((Message msg) {
            if (msg.content.length == 8185) {
                assert(msg.content == BorderlineReply);
                foundVerbatim = true;
            }
        }, (_) {});
    }
    assert(foundVerbatim);
}

// A6/H4 quirk (documented, not fixed): when the purge empties the chat below
// the compression floor, compress returns WITHOUT setHistory — the purge was
// applied only to the local copy, so nothing was actually evicted and no
// checkpoint fires.
unittest {
    Chat chat;
    chat.setSystemPrompt("sys");
    chat.addUserQuery("q1");
    chat.add(ToolMessage("", JSONValue([makeToolCall("toolA", "call1")]),
            JSONValue.init, JSONValue.init));
    chat.add(ToolResponse("resp1", "call1", "toolA", true));
    chat.add(Message(Role.assistant, userQuery: false, content: "a1", thinking: null));
    assert(chat.getMessages.length == 5);

    int events = 0;
    auto agent = makeTestSummaryAgent();
    agent.addCheckpointListener((const SummaryAgent.CompressionCheckpoint cp) {
        events++;
    });

    auto result = agent.compress(chat, null, ["toolA"]);
    assert(!result.compressed);
    assert(result.purgedCount == 2);
    assert(events == 0);
    assert(chat.getMessages.length == 5); // untouched by the local purge
}

// G2/A6: the seam is multicast — two registered listeners BOTH receive the
// event — and a throwing listener is swallowed and trace-logged, so
// compression completes normally (an indexing failure must never break
// compression).
unittest {
    import std.array : replicate;

    immutable OversizedReply = "x".replicate(9000);
    Chat chat;
    chat.setSystemPrompt("sys");
    foreach (turn; 1 .. 9) {
        chat.addUserQuery("q" ~ turn.to!string);
        const reply = (turn == 5) ? OversizedReply : "ok";
        chat.add(Message(Role.assistant, userQuery: false, content: reply, thinking: null));
    }

    int firstEvents = 0;
    int throwingEvents = 0;
    auto agent = makeTestSummaryAgent();
    agent.addCheckpointListener((const SummaryAgent.CompressionCheckpoint cp) {
        firstEvents++;
    });
    agent.addCheckpointListener((const SummaryAgent.CompressionCheckpoint cp) {
        throwingEvents++;
        throw new Exception("listener boom");
    });
    auto result = agent.compress(chat);

    assert(result.compressed);
    assert(firstEvents == 1);
    assert(throwingEvents == 1); // reached the listener before throwing
}

// A6 default path: with no listener registered the checkpoint falls back to
// a structured trace dump and compression completes normally.
unittest {
    import std.array : replicate;

    immutable OversizedReply = "x".replicate(9000);
    Chat chat;
    chat.setSystemPrompt("sys");
    foreach (turn; 1 .. 9) {
        chat.addUserQuery("q" ~ turn.to!string);
        if (turn < 8) {
            const reply = (turn == 5) ? OversizedReply : "ok";
            chat.add(Message(Role.assistant, userQuery: false, content: reply, thinking: null));
        }
    }

    auto agent = makeTestSummaryAgent();
    agent.setCheckpointSessionId("sess-trace");
    auto result = agent.compress(chat);

    assert(result.compressed);
    assert(result.newLength == 6);
}

// H3: an oversized Y message is replaced in place by summarizeSingleMessage;
// the pre-replacement original is reported in evictedInPlace, and the
// replacement keeps the original saveData and turnId (the latent
// metadata-loss bug is fixed). The LLM is unreachable (empty URL), so the
// replacement comes from the truncation path — the request retry backoff
// (~3.5s) dominates this test's runtime.
unittest {
    import std.array : replicate;

    immutable OversizedReply = "x".replicate(9000);
    Chat chat;
    chat.setSystemPrompt("sys");
    chat.addUserQuery("q1");
    chat.add(Message(Role.assistant, userQuery: false, content: "a1", thinking: null));
    chat.addUserQuery("q2");
    chat.add(Message(Role.assistant, userQuery: false, content: OversizedReply,
            thinking: null, saveData: JSONValue(["note": JSONValue(42)])));
    chat.addUserQuery("q3");
    chat.add(Message(Role.assistant, userQuery: false, content: "a3", thinking: null));
    assert(chat.getMessages.length == 7);

    int events = 0;
    long turnStart = -1;
    long turnEnd = -1;
    Chat.MessageT[] inPlace; // cast() away const below: the test only reads it
    auto agent = makeTestSummaryAgent();
    agent.addCheckpointListener((const SummaryAgent.CompressionCheckpoint cp) {
        events++;
        turnStart = cp.turnStart;
        turnEnd = cp.turnEnd;
        inPlace = cast(Chat.MessageT[]) cp.evictedInPlace.dup;
    });

    auto result = agent.compress(chat);

    assert(result.compressed);
    assert(events == 1);
    assert(turnStart == 2);
    assert(turnEnd == 2);
    assert(inPlace.length == 1);
    inPlace[0].match!((Message m) {
        assert(m.content.length == 9000);
        assert(m.turnId == 2);
    }, (_) { assert(false, "expected the original oversized a2"); });
    // the replacement kept the original saveData and turnId (H3)
    assert(chat.getMessages.length == 7);
    chat.getMessages[4].match!((Message m) {
        assert(m.turnId == 2);
        assert(m.saveData["note"].integer == 42);
        assert(m.content.length == agent.TokenBudget); // truncation path
    }, (_) { assert(false, "expected the truncated replacement"); });
    // L4 (I-2): the replacement's saveData is an independent copy — mutating
    // it in place must not affect the checkpoint payload's original.
    chat.getMessages[4].match!((Message m) { m.saveData["mutated"] = true; }, (_) {
        assert(false, "expected the truncated replacement");
    });
    inPlace[0].match!((Message m) {
        assert(m.saveData["note"].integer == 42);
        assert(("mutated" in m.saveData) is null,
            "payload original must not alias the live replacement");
    }, (_) { assert(false, "expected the original oversized a2"); });
    // I1: the compressed history stays sorted
    long prev = 0;
    foreach (m; chat.getMessages) {
        const id = turnIdOf(m);
        assert(id >= prev);
        prev = id;
    }
}

// Task 7: a purge-tools compression preserves every remaining message's
// TurnID and I1 ordering. Each survivor is a kept copy of a pre-compression
// message (matched by marker) and must carry the same stamp it had before;
// the purge must not shift or re-stamp anything.
unittest {
    Chat chat;
    chat.setSystemPrompt("sys");
    chat.addUserQuery("q1"); // turn 1
    chat.add(ToolMessage("", JSONValue([makeToolCall("toolA", "call1")]),
            JSONValue.init, JSONValue.init));
    chat.add(ToolResponse("resp1", "call1", "toolA", true));
    chat.add(Message(Role.assistant, userQuery: false, content: "a1", thinking: null));
    chat.addUserQuery("q2"); // turn 2
    chat.add(ToolMessage("", JSONValue([makeToolCall("toolB", "call2")]),
            JSONValue.init, JSONValue.init));
    chat.add(ToolResponse("resp2", "call2", "toolB", true));
    chat.add(Message(Role.assistant, userQuery: false, content: "a2", thinking: null));
    chat.addUserQuery("q3"); // turn 3
    chat.add(Message(Role.assistant, userQuery: false, content: "a3", thinking: null));
    assert(chat.getMessages.length == 11);

    // A stable per-message marker: "msg:" + content for plain messages,
    // "tm:" + first tool-call id for ToolMessages, "tr:" + call id for
    // ToolResponses, "vis:" + content for vision. The type prefix keeps a
    // kept ToolMessage distinguishable from the ToolResponse carrying the
    // same call id (tmB vs resp2 both use "call2").
    string markerOf(Chat.MessageT m) {
        string marker;
        m.match!((Message x) { marker = "msg:" ~ x.content; }, (ToolMessage x) {
            marker = "tm:";
            if (x.toolCalls.type == JSONType.array && !x.toolCalls.array.empty)
                marker ~= x.toolCalls.array[0]["id"].str;
        }, (ToolResponse x) { marker = "tr:" ~ x.toolCallId; }, (VisionMessage x) {
            marker = "vis:" ~ x.content;
        });
        return marker;
    }

    // Pre-compression stamps keyed by marker.
    long[string] idByMarker;
    foreach (m; chat.getMessages) {
        idByMarker[markerOf(m)] = turnIdOf(m);
    }

    // Fixture geometry: after the purge allMessages = [sys, q1, a1, q2, tmB,
    // resp2, a2, q3, a3] (9 entries). Y = last KeepLast (5) = [tmB..a3];
    // the candidates [q1, a1, q2] are tiny and all fit the X budget, so
    // remaining is empty - the purge is the only eviction.
    auto agent = makeTestSummaryAgent();
    auto result = agent.compress(chat, null, ["toolA"]);

    assert(result.compressed);
    assert(result.purgedCount == 2); // the toolA ToolMessage + its ToolResponse

    // Survivors: system, q1, a1, q2, tm(toolB), resp2, a2, q3, a3.
    auto msgs = chat.getMessages;
    assert(msgs.length == 9);

    long prev = 0;
    foreach (m; msgs) {
        const marker = markerOf(m);
        assert(marker in idByMarker, "every survivor must be a kept original");
        const id = turnIdOf(m);
        assert(id == idByMarker[marker], "kept messages must keep their TurnID");
        assert(id >= prev, "history must stay I1-sorted after the purge");
        prev = id;
    }
    // The kept toolB traffic stays in turn 2, the tail in turn 3.
    assert(turnIdOf(msgs[4]) == 2); // tm(toolB)
    assert(turnIdOf(msgs[5]) == 2); // resp2
    assert(turnIdOf(msgs[7]) == 3); // q3
    assert(turnIdOf(msgs[8]) == 3); // a3
}

// --- Task 9 spike: Phase-1 seam validation (P3, optional) ---
// The seam is validated from the consumer's side (the producer's side was
// Task 5). A mock indexing listener stands in for the Phase-1 async indexer:
// it subscribes once, owns copies of every payload, and records each
// delivery in order. Phase 0 implements no indexing of any kind — the mock
// only proves the seam hands a Phase-1 consumer complete, correctly-ordered
// checkpoints it can rely on.

// One checkpoint delivery as recorded by the mock Phase-1 indexing consumer.
// The payload arrays are owned copies (.dup already yields mutable arrays —
// the mock only reads them), so the records stay valid after the live chat
// is rewritten by later compressions.
private struct SpikeCheckpointRecord {
    SysTime timestamp;
    string sessionId;
    long turnStart;
    long turnEnd;
    Chat.MessageT[] evictedSummarized;
    Chat.MessageT[] evictedPurged;
    Chat.MessageT[] evictedInPlace;
    string summaryText;
    size_t originalLength;
    size_t newLength;
    long newContextSize;

    static SpikeCheckpointRecord of(const SummaryAgent.CompressionCheckpoint cp) {
        return SpikeCheckpointRecord(cp.timestamp, cp.sessionId, cp.turnStart,
                cp.turnEnd, cp.evictedSummarized.dup, cp.evictedPurged.dup,
                cp.evictedInPlace.dup, cp.summaryText, cp.originalLength,
                cp.newLength, cp.newContextSize);
    }
}

// End-to-end spike: one session compressed twice. The mock consumer receives
// every checkpoint in delivery order with complete payloads — turn ranges
// strictly advancing and contiguous (nothing evicted twice, nothing skipped),
// the session id on every event, evicted arrays matching the discarded turns,
// timestamps non-decreasing, and each payload internally in canonical order.
// A throwing sibling listener is registered first: the seam swallows it and
// the mock consumer still receives everything (the throwing-listener path
// must never break an indexing consumer). The consumer's dump (counts,
// summary text, session ids) mirrors the first step a Phase-1 indexer takes.
unittest {
    import std.array : replicate;

    auto agent = makeTestSummaryAgent();
    agent.setCheckpointSessionId("sess-spike");

    // Throwing sibling registered BEFORE the mock consumer: the seam must
    // catch it and still deliver to the consumer.
    int throwingEvents = 0;
    agent.addCheckpointListener((const SummaryAgent.CompressionCheckpoint cp) {
        throwingEvents++;
        throw new Exception("indexing listener boom");
    });

    SpikeCheckpointRecord[] records;
    agent.addCheckpointListener((const SummaryAgent.CompressionCheckpoint cp) {
        records ~= SpikeCheckpointRecord.of(cp);
    });

    immutable OversizedReply = "x".replicate(9000); // 9000 chars / ApproxTokenSize = 4500 tokens

    // Session phase 1: turns 1-8, the turn-5 reply oversized and turn 8
    // query-only — the geometry evicts exactly turns 1-5.
    Chat chat;
    chat.setSystemPrompt("sys");
    foreach (turn; 1 .. 9) {
        chat.addUserQuery("q" ~ turn.to!string);
        if (turn < 8) {
            const reply = (turn == 5) ? OversizedReply : "ok";
            chat.add(Message(Role.assistant, userQuery: false, content: reply, thinking: null));
        }
    }
    assert(chat.getMessages.length == 16);

    auto first = agent.compress(chat);
    assert(first.compressed);
    assert(first.newLength == 1 + agent.KeepLast);
    assert(records.length == 1);
    assert(throwingEvents == 1);
    assert(records[0].sessionId == "sess-spike");
    assert(records[0].turnStart == 1);
    assert(records[0].turnEnd == 5);
    assert(records[0].originalLength == 16);
    assert(records[0].newLength == 1 + agent.KeepLast);
    assert(records[0].evictedSummarized.length == 10);
    assert(records[0].evictedPurged.empty);
    assert(records[0].evictedInPlace.empty);
    assert(records[0].summaryText.empty); // all summary chunks fail without an LLM

    // Session phase 2: continue the SAME session — the counter must not have
    // been reset by compression (q9 opens turn 9, not turn 1) — and compress
    // again. The same geometry now evicts exactly turns 6-10.
    chat.add(Message(Role.assistant, userQuery: false, content: "a8", thinking: null));
    foreach (turn; 9 .. 14) {
        chat.addUserQuery("q" ~ turn.to!string);
        if (turn < 13) {
            const reply = (turn == 10) ? OversizedReply : "ok";
            chat.add(Message(Role.assistant, userQuery: false, content: reply, thinking: null));
        }
    }
    assert(chat.getMessages.length == 16);
    assert(turnIdOf(chat.getMessages[7]) == 9); // q9 — continued, not reset

    auto second = agent.compress(chat);
    assert(second.compressed);
    assert(records.length == 2);
    assert(throwingEvents == 2);
    assert(records[1].sessionId == "sess-spike");
    assert(records[1].turnStart == 6);
    assert(records[1].turnEnd == 10);
    assert(records[1].evictedSummarized.length == 10);
    assert(records[1].originalLength == 16);
    assert(records[1].newLength == 1 + agent.KeepLast);
    assert(records[1].evictedPurged.empty);
    assert(records[1].evictedInPlace.empty);
    assert(records[1].summaryText.empty);

    // The consumer's ordering guarantees:
    // - delivery order matches eviction order (turn ranges strictly advance)
    // - the evicted turn ranges are contiguous — a complete session prefix
    //   with no gaps or double evictions for the indexer
    // - timestamps never run backwards
    assert(records[0].turnStart < records[1].turnStart);
    assert(records[0].turnEnd == records[1].turnStart - 1);
    assert(records[0].timestamp <= records[1].timestamp);

    // Per-event payload integrity: the evicted slice's own turn range equals
    // the reported range and is internally in canonical order.
    foreach (r; records) {
        const range = turnRangeOf(r.evictedSummarized);
        assert(range.turnStart == r.turnStart);
        assert(range.turnEnd == r.turnEnd);
        long prev = 0;
        foreach (m; r.evictedSummarized) {
            const id = turnIdOf(m);
            assert(id >= prev, "evicted payload must be in canonical order");
            prev = id;
        }
    }

    // The accumulated copies stay intact after the live chat was rewritten by
    // the second compression (each payload was .dup'd before firing).
    records[0].evictedSummarized[0].match!((Message m) {
        assert(m.content == "q1");
        assert(m.turnId == 1);
    }, (_) { assert(false, "expected q1"); });
    records[0].evictedSummarized[$ - 1].match!((Message m) {
        assert(m.content.length == 9000);
        assert(m.turnId == 5);
    }, (_) { assert(false, "expected the oversized a5"); });
    records[1].evictedSummarized[0].match!((Message m) {
        assert(m.content == "q6");
        assert(m.turnId == 6);
    }, (_) { assert(false, "expected q6"); });
    records[1].evictedSummarized[$ - 1].match!((Message m) {
        assert(m.content.length == 9000);
        assert(m.turnId == 10);
    }, (_) { assert(false, "expected the oversized a10"); });

    // The live history after both compressions keeps only the un-evicted
    // tail (q11..q13), with untouched TurnIDs intact.
    assert(chat.getMessages.length == 6);
    assert(turnIdOf(chat.getMessages[1]) == 11);

    // The consumer-visible dump: counts, summary text and session ids per
    // checkpoint, in delivery order (the Phase-1 indexer's first step).
    foreach (i, r; records) {
        logger.tracef(
                "spike checkpoint %s: session=%s turns=%s..%s summarized=%s purged=%s inPlace=%s summaryChars=%s",
                i, r.sessionId, r.turnStart,
                r.turnEnd, r.evictedSummarized.length, r.evictedPurged.length,
                r.evictedInPlace.length, r.summaryText.length);
    }
}

// Seam-level delivery check: every payload field the seam hands out reaches
// the consumer intact. compress() can only produce an empty summaryText
// without an LLM, so this drives fireCheckpoint directly (the delivery seam
// behind the listener interface, private to this module) with a synthetic
// checkpoint carrying non-empty summary text and all three evicted arrays.
// No indexing: the consumer only records what it received.
unittest {
    auto agent = makeTestSummaryAgent();

    SpikeCheckpointRecord[] seen;
    agent.addCheckpointListener((const SummaryAgent.CompressionCheckpoint cp) {
        seen ~= SpikeCheckpointRecord.of(cp);
    });

    SummaryAgent.CompressionCheckpoint synthetic;
    synthetic.timestamp = Clock.currTime;
    synthetic.sessionId = "sess-spike";
    synthetic.turnStart = 1;
    synthetic.turnEnd = 2;
    synthetic.summaryText = "- turns 1-2 discussed TurnID stamping";
    synthetic.originalLength = 9;
    synthetic.newLength = 4;
    synthetic.newContextSize = 100;
    synthetic.evictedSummarized = [
        Chat.MessageT(Message(Role.user, userQuery: true, content: "q1", thinking: null))
    ];
    synthetic.evictedPurged = [
        Chat.MessageT(ToolMessage("", JSONValue([makeToolCall("toolA",
                    "call1")]), JSONValue.init, JSONValue.init))
    ];
    synthetic.evictedInPlace = [
        Chat.MessageT(Message(Role.assistant, userQuery: false, content: "a2", thinking: null))
    ];
    agent.fireCheckpoint(synthetic);

    assert(seen.length == 1);
    assert(seen[0].sessionId == "sess-spike");
    assert(seen[0].turnStart == 1);
    assert(seen[0].turnEnd == 2);
    assert(seen[0].summaryText == synthetic.summaryText);
    assert(seen[0].originalLength == 9);
    assert(seen[0].newLength == 4);
    assert(seen[0].newContextSize == 100);
    assert(seen[0].timestamp == synthetic.timestamp);
    assert(seen[0].evictedSummarized.length == 1);
    assert(seen[0].evictedPurged.length == 1);
    assert(seen[0].evictedInPlace.length == 1);
    seen[0].evictedInPlace[0].match!((Message m) { assert(m.content == "a2"); }, (_) {
        assert(false, "expected a2");
    });
}
