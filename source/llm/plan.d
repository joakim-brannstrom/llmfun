module llm.plan;

import logger = std.logger;

import my.filter : ReFilter;

import llm.agent : Agent;
import llm.config : LlmConfig, promptToPath;
import llm.pipeline : Pipeline, PipelineResult, pipelineBuilder;
import llm.rag.rag : RAG;
import llm.metric.monitor : MetricMonitor;
import llm.utility : SystemPromptInit;

/// Runs a two-stage pipeline: System Designer → Implementation Planner
///
/// Agent 1 (System Designer): calls getThinkingTemplate("system_design"),
///     analyzes the user query, produces a system design, and saves it to
///     plan/system_design.md via writeFile.
///
/// Agent 2 (Implementation Planner): calls getThinkingTemplate("implementation_plan"),
///     reads the system design, converts it into an implementation plan with
///     individual tasks, and saves it to plan/implementation_plan.md via writeFile.
///
/// Both agents are transient and only exist for the duration of the pipeline.
PipelineResult runPlanPipeline(string query, LlmConfig llmConf, RAG rag,
        MetricMonitor monitor, bool delegate() interrupt = null, ReFilter toolFilter) {

    // dfmt off
    // --- Agent 1: System Designer ---
    const systemDesignerPrompt =
        "You are a System Designer. Your job is to analyze a user's request and produce a\n" ~
        "comprehensive system design document.\n\n" ~
        "## Instructions\n" ~
        "1. Call `getThinkingTemplate(\"system_design\")` to get a structured thinking framework.\n" ~
        "2. Follow the template steps to analyze the user's request thoroughly.\n" ~
        "3. Produce a clear, well-structured system design document.\n" ~
        "4. Save your design document to a file in the `plan/` directory using `writeFile`.\n" ~
        "   Use the filename format: `plan/system_design.md`.\n" ~
        "5. After saving, call 'setPipelineOutput' with 'done' and call `taskDone` to complete your task.\n";

    const systemDesignerFeedbackPrompt =
        "You are a System Design Reviewer. Your job is to review a system design plan, critique it, and provide actionable feedback for improvement.\n\n" ~
        "## Instructions\n" ~
        "1. Read the system design document from `plan/system_design.md` using `readFile`.\n" ~
        "2. Call `getThinkingTemplate(\"system_design\")` to get a structured framework for reviewing designs.\n" ~
        "3. Follow the template to thoroughly analyze the design across dimensions such as requirements clarity, architecture, scalability, reliability, security, cost-efficiency, maintainability, and documentation quality.\n" ~
        "4. Produce a detailed review document that:\n" ~
        "   - Summarizes strengths.\n" ~
        "   - Identifies specific weaknesses, risks, or missing elements.\n" ~
        "   - Provides concrete, actionable suggestions for improvement.\n" ~
        "5. Call 'pipelineOutput' with your review as argument.\n" ~
        "6. After call to 'pipelineOutput', call `taskDone` to complete your task.\n";

    // --- Agent 2: Implementation Planner ---
    const implPlannerPrompt =
        "You are an Implementation Planner. Your job is to convert a system design into a\n" ~
        "detailed, actionable implementation plan with individual tasks.\n\n" ~
        "## Instructions\n" ~
        "1. Call `getThinkingTemplate(\"implementation_plan\")` to get a structured thinking framework.\n" ~
        "2. Read the system design document from `plan/system_design.md` using `readFile`.\n" ~
        "3. Follow the template steps to break the design into concrete implementation tasks.\n" ~
        "4. Save your implementation plan to `plan/implementation_plan.md` using `writeFile`.\n" ~
        "5. After saving, call 'setPipelineOutput' with 'done' and call `taskDone` to complete your task.\n";
    // dfmt on

    // Create transient agents
    auto designer = new Agent("system_designer", llmConf, monitor, rag, toolFilter);
    designer.setSystemPrompt(
            SystemPromptInit(llmConf.promptToPath(llmConf.codeModel.prompt)).toString);
    designer.addUserQuery(systemDesignerPrompt);

    auto designReview = new Agent("system_design_review", llmConf, monitor, rag, toolFilter);
    designReview.setSystemPrompt(
            SystemPromptInit(llmConf.promptToPath(llmConf.codeModel.prompt)).toString);
    designReview.addUserQuery(systemDesignerFeedbackPrompt);

    auto planner = new Agent("implementation_planner", llmConf, monitor, rag, toolFilter);
    planner.setSystemPrompt(
            SystemPromptInit(llmConf.promptToPath(llmConf.codeModel.prompt)).toString);
    planner.addUserQuery(implPlannerPrompt);

    // Wire into a linear pipeline (no loops)
    // dfmt off
    auto pipeline = pipelineBuilder
        .addNode("system_design", designer)
        .addNode("system_design_review", designReview)
        .addNode("impl_planner", planner)
        .addEdge("system_design", "system_design_review", null, 1)
        .addEdge("system_design_review", "system_design", null, 0)
        .addEdge("system_design", "impl_planner")
        .startNode("system_design")
        .stopNode("impl_planner").build;
    // dfmt on

    logger.trace(pipeline);
    logger.infof("[plan] Starting pipeline for query: %s", query);
    auto result = pipeline.run(query, interrupt);

    logger.infof("[plan] Pipeline completed: allSuccess=%s, agents=%d",
            result.allSuccess, result.agentResults.length);

    return result;
}
