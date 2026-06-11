module llm.coder;

import logger = std.logger;
import std.file : exists;

import my.filter : ReFilter;

import llm.agent : Agent;
import llm.config : LlmConfig, promptToPath;
import llm.metric.monitor : MetricMonitor;
import llm.pipeline : Pipeline, PipelineResult, pipelineBuilder;
import llm.rag.rag : RAG;
import llm.utility : SystemPromptInit;

/// Runs a coder-reviewer loop pipeline.
///
/// Agent 1 (Coder): implements code based on the user's request,
///     saves output to `code/implementation.md`, and incorporates
///     reviewer feedback on subsequent iterations.
///
/// Agent 2 (Code Reviewer): reviews the coder's output using
///     `getThinkingTemplate("code_review")`, produces structured
///     feedback, and passes it back via `pipelineOutput`.
///
/// The loop runs up to three times:
///   1. Coder implements → Reviewer reviews
///   2. Coder revises → Reviewer reviews
///   3. Coder finalizes (pipeline stops)
///
/// Both agents are transient and only exist for the duration of the pipeline.
PipelineResult runCoderPipeline(string query, LlmConfig llmConf, RAG rag,
        MetricMonitor monitor, bool delegate() interrupt = null, ReFilter toolFilter) {

    // dfmt off

    // Agent 1: Coder
    auto codeQuery =
        "You are a Coder. Your job is to implement working code based on the user's request.\n\n" ~
        "## Instructions\n" ~
        "1. Analysis of the source code is available via the query tools in the first database return from `listRAGDatabases`. Use it to better understand the source code.\n" ~
        "2. Call `getThinkingTemplate(\"code_task\")` to get a structured thinking framework.\n" ~
        "3. Analyze the user's request and plan your implementation.\\n" ~
        "4. Write clean, well-structured code.\n" ~
        "5. Save your implementation to `code/implementation.md` using `writeFile`.\n" ~
        "6. If you receive feedback from a code reviewer (passed as input), address each point in your revision.\n" ~
        "7. After saving, call `pipelineOutput` with your implementation summary and call `taskDone` to complete your task.\n";

    // Agent 2: Code Reviewer
    const reviewerQuery =
        "You are a Code Reviewer. Your job is to review the coder's implementation and provide actionable feedback.\n\n" ~
        "## Instructions\n" ~
        "1. Analysis of the source code is available via the query tools in the first database return from `listRAGDatabases`. Use it to better understand the source code.\n" ~
        "2. Read the implementation from `code/implementation.md` using `readFile`.\n" ~
        "3. Call `getThinkingTemplate(\"code_review\")` to get a structured framework for reviewing code.\n" ~
        "4. Follow the template to thoroughly analyze the code for bugs, security issues, style violations, performance problems, and improvements.\n" ~
        "5. Produce a detailed review that:\n" ~
        "   - Summarizes what works well.\n" ~
        "   - Identifies specific issues with line numbers or code snippets.\n" ~
        "   - Provides concrete, actionable suggestions for each issue.\n" ~
        "6. Call `pipelineOutput` with your review feedback as argument.\n" ~
        "7. After calling `pipelineOutput`, call `taskDone` to complete your task.\n";
    // dfmt on

    // Create transient agents
    auto coder = new Agent("coder", llmConf, monitor, rag, toolFilter);
    coder.setSystemPrompt(SystemPromptInit(llmConf.promptToPath(llmConf.agentPrompt)).toString);
    if ((llmConf.workArea ~ "plan/code_analysis.md").exists) {
        codeQuery ~= "Note: an analysis of the source code and project is available via the query tools. Use it when unclear about details in the source code.\n";
    }
    coder.addUserQuery(codeQuery);
    coder.addUserQuery(query);

    auto reviewer = new Agent("code_reviewer", llmConf, monitor, rag, toolFilter);
    reviewer.setSystemPrompt(SystemPromptInit(llmConf.promptToPath(llmConf.agentPrompt)).toString);
    reviewer.addUserQuery(reviewerQuery);

    // Wire into a loop pipeline: coder -> reviewer -> coder (up to 3 coder runs)
    // maxLoops=2 on the feedback edge means the reviewer feeds back to the coder
    // at most 2 times, resulting in 3 total coder executions.
    // dfmt off
    auto pipelineBuilder = pipelineBuilder
        .addNode("coder", coder)
        .addNode("reviewer", reviewer)
        .addEdge("coder", "reviewer")
        .addEdge("reviewer", "coder", 1u)
        .addEdge("reviewer", "done", 0)
        .startNode("coder")
        .stopNode("done");
    // dfmt on
    auto pipeline = pipelineBuilder.build();

    logger.trace(pipeline);
    logger.infof("[coder] Starting coder-reviewer pipeline for query: %s", query);
    auto result = pipeline.run(interrupt);

    logger.infof("[coder] Pipeline completed: allSuccess=%s, agents=%d",
            result.allSuccess, result.agentResults.length);

    return result;
}
