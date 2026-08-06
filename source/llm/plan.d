module llm.plan;

import logger = std.logger;
import std.conv : text;
import std.datetime : Clock, dur;
import std.file : exists, timeLastModified;

import my.filter : ReFilter;

import llm.agent : Agent;
import llm.config : LlmConfig;
import llm.metric.monitor : MetricMonitor;
import llm.pipeline : Pipeline, PipelineResult, pipelineBuilder, NodeConfig;
import llm.rag.rag : RAG;
import llm.skill : makeSkillManager;
import llm.types : IStreamCallback;

/// Runs a two-stage pipeline: System Designer → Implementation Planner
///
/// Agent 1 (System Designer): calls loadSkill('system-design', ...),
///     analyzes the user query, produces a system design, and saves it to
///     plan/system_design.md via writeFile.
///
/// Agent 2 (Implementation Planner): calls loadSkill('implementation-plan', ...),
///     reads the system design, converts it into an implementation plan with
///     individual tasks, and saves it to plan/implementation_plan.md via writeFile.
///
/// Both agents are transient and only exist for the duration of the pipeline.
PipelineResult runPlanPipeline(string query, LlmConfig llmConf, RAG rag, MetricMonitor monitor,
        bool delegate() interrupt, ReFilter toolFilter, IStreamCallback callback) {

    // dfmt off
    // Agent 1: System Designer
    const systemDesignerPrompt =
        "You are a System Designer. Your job is to analyze a user's request and produce a comprehensive system design document.\n\n" ~
        "## Instructions\n" ~
        "1. The source code may exist in the primary RAG database. Use it when unclear about details in the source code that you need to analyze.\n" ~
        "2. Call `loadSkill(\"system-design\", \"skills/system-design\")` to load the system design skill.\n" ~
        "3. Follow the template steps to analyze the user's request thoroughly.\n" ~
        "4. Produce a clear, well-structured system design document.\n" ~
        "5. Save your design document to a file in the `plan/system_design.md`.\n" ~
        "6. After saving, call 'setPipelineOutput' with 'ok' and call `taskDone` to complete your task.\n";

    // Agent 2: System Design reviewer
    // TODO: bullet 3 should reference a system design review template
    const systemDesignerFeedbackPrompt =
        "You are a System Design Reviewer. Your job is to review a system design plan, critique it, and provide actionable feedback for improvement.\n\n" ~
        "## Instructions\n" ~
        "1. The source code may exist in the primary RAG database. Use it when unclear about details in the source code that you need to analyze.\n" ~
        "2. Read the system design document from `plan/system_design.md` using `readFile` and understand the task the user gave the designer agent.\n" ~
        "3. Call `loadSkill(\"system-design\", \"skills/system-design\")` to load the system design skill for review guidance.\n" ~
        "4. Follow the template to thoroughly analyze the design across dimensions such as requirements clarity, architecture, scalability, reliability, security, cost-efficiency, maintainability, and documentation quality.\n" ~
        "5. Produce a detailed review document that:\n" ~
        "   - Summarizes strengths.\n" ~
        "   - Identifies specific weaknesses, risks, or missing elements.\n" ~
        "   - Provides concrete, actionable suggestions for improvement.\n" ~
        "6. Call 'pipelineOutput' with your review as argument.\n" ~
        "7. After call to 'pipelineOutput', call `taskDone` to complete your task.\n";

    // Agent 3: Implementation Planner
    const implPlannerPrompt =
        "You are an Implementation Planner. Your job is to convert a system design into a\n" ~
        "detailed, actionable implementation plan with individual tasks.\n\n" ~
        "## Instructions\n" ~
        "1. The source code may exist in the primary RAG database. Use it when unclear about details in the source code that you need to analyze.\n" ~
        "2. Call `loadSkill(\"implementation-plan\", \"skills/implementation-plan\")` to load the implementation plan skill.\n" ~
        "3. Read the system design document from `plan/system_design.md` using `readFile`.\n" ~
        "4. Follow the template steps to break the design into concrete implementation tasks.\n" ~
        "5. Save your implementation plan to `plan/implementation_plan.md` using `writeFile`.\n" ~
        "6. After saving, call 'setPipelineOutput' with 'done' and call `taskDone` to complete your task.\n";
    // dfmt on

    auto tmpManager = makeSkillManager(llmConf);

    auto designer = new Agent("system_designer", llmConf, monitor, rag, toolFilter);
    designer.setSystemPrompt(llmConf.getPrompt(tmpManager));
    designer.addUserQuery(systemDesignerPrompt);
    designer.addUserQuery(query);

    auto designReview = new Agent("system_design_review", llmConf, monitor, rag, toolFilter);
    designReview.setSystemPrompt(llmConf.getPrompt(tmpManager));
    designReview.addUserQuery(systemDesignerFeedbackPrompt);
    designReview.addUserQuery(i"The users task for the system_designer agent was the following:\n\n---\n\n$(
            query)\n\n---\n".text);

    auto planner = new Agent("implementation_planner", llmConf, monitor, rag, toolFilter);
    planner.setSystemPrompt(llmConf.getPrompt(tmpManager));
    planner.addUserQuery(implPlannerPrompt);

    tmpManager = null; // release back to GC

    // dfmt off
    auto pipelineBuilder = pipelineBuilder
        .streamCallback(callback)
        .addNode("system_design", designer)
        .addNode("system_design_review", designReview)
        .addNode("impl_planner", planner)
        .addEdge("system_design", "system_design_review", null, 1)
        .addEdge("system_design_review", "system_design", null, 0)
        .addEdge("system_design", "impl_planner")
        .startNode("system_design")
        .stopNode("impl_planner");
    // dfmt on

    auto pipeline = pipelineBuilder.build();

    logger.trace(pipeline);
    logger.infof("plan Starting pipeline for query: %s", query);
    auto result = pipeline.run(interrupt);

    logger.infof("plan Pipeline completed: success=%s agents=%s",
            result.allSuccess, result.agentResults.length);

    return result;
}
