module llm.coder;

import logger = std.logger;
import std.file : exists;

import my.filter : ReFilter;

import llm.agent : Agent;
import llm.config : LlmConfig;
import llm.metric.monitor : MetricMonitor;
import llm.pipeline : Pipeline, PipelineResult, pipelineBuilder;
import llm.rag.rag : RAG;
import llm.skill : makeSkillManager;
import llm.types : IStreamCallback;

/// Runs a coder-reviewer loop pipeline.
///
/// Agent 1 (Coder): implements code based on the user's request,
///     saves output to `code/implementation.md`, and incorporates
///     reviewer feedback on subsequent iterations.
///
/// Agent 2 (Code Reviewer): reviews the coder's output using
///     `loadSkill('code-review', ...)`, produces structured
///     feedback, and passes it back via `pipelineOutput`.
///
/// The loop runs up to three times:
///   1. Coder implements → Reviewer reviews
///   2. Coder revises → Reviewer reviews
///   3. Coder finalizes (pipeline stops)
///
/// Both agents are transient and only exist for the duration of the pipeline.
PipelineResult runCoderPipeline(string query, LlmConfig llmConf, RAG rag, MetricMonitor monitor,
        bool delegate() interrupt, ReFilter toolFilter, IStreamCallback callback) {

    // dfmt off
    // Agent 1: Coder
    auto codeQuery =
        "You are a Coder. Your job is to implement working code based on the user's request.\n\n" ~
        "## Instructions\n" ~
        "1. The source code may exist in the primary RAG database. Use it when unclear about details in the source code that you need to analyze.\n" ~
        "2. Call `loadSkill(\"code-task\", \"skills/code-task\")` to load the code task skill.\n" ~
        "3. Analyze the user's request and plan your implementation.\n" ~
        "4. Write clean, well-structured code.\n" ~
        "5. Save your implementation to `code/implementation.md` using `writeFile`.\n" ~
        "6. If you receive feedback from a code reviewer (passed as input), address each point in your revision.\n" ~
        "7. After saving, call `pipelineOutput` with your implementation summary and call `taskDone` to complete your task.\n";

    // Agent 2: Code Reviewer
    const reviewerQuery =
        "You are a Code Reviewer. Your job is to review the coder's implementation and provide actionable feedback.\n\n" ~
        "## Instructions\n" ~
        "1. The source code may exist in the primary RAG database. Use it when unclear about details in the source code that you need to analyze.\n" ~
        "   Note: the state of the source code in the primary RAG database do not contain the latest changes. You have to read the files manually to see the latest changes.\n" ~
        "2. Read the implementation from `code/implementation.md` using `readFile`.\n" ~
        "3. Call `loadSkill(\"code-review\", \"skills/code-review\")` to load the code review skill.\n" ~
        "4. Follow the template to thoroughly analyze the code for bugs, security issues, style violations, performance problems, and improvements.\n" ~
        "5. Produce a detailed review that:\n" ~
        "   - Summarizes what works well.\n" ~
        "   - Identifies specific issues with line numbers or code snippets.\n" ~
        "   - Provides concrete, actionable suggestions for each issue.\n" ~
        "6. Call `pipelineOutput` with your review feedback as argument.\n" ~
        "7. After calling `pipelineOutput`, call `taskDone` to complete your task.\n";
    // dfmt on

    auto tmpManager = makeSkillManager(llmConf);

    auto coder = new Agent("coder", llmConf, monitor, rag, toolFilter);
    coder.setSystemPrompt(llmConf.getPrompt(tmpManager));
    coder.addUserQuery(query);

    auto reviewer = new Agent("code_reviewer", llmConf, monitor, rag, toolFilter);
    reviewer.setSystemPrompt(llmConf.getPrompt(tmpManager));
    reviewer.addUserQuery(reviewerQuery);

    tmpManager = null; // release back to GC

    // dfmt off
    auto pipelineBuilder = pipelineBuilder
        .streamCallback(callback)
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
    logger.infof("coder Starting pipeline for query: %s", query);
    auto result = pipeline.run(interrupt);

    logger.infof("coder Pipeline completed: success=%s agents=%s",
            result.allSuccess, result.agentResults.length);

    return result;
}
