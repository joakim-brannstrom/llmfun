module llm.plan;

import logger = std.logger;

import my.filter : ReFilter;
import std.file : exists, timeLastModified;
import std.datetime : Clock, dur;

import llm.agent : Agent;
import llm.config : LlmConfig, promptToPath;
import llm.pipeline : Pipeline, PipelineResult, pipelineBuilder, NodeConfig;
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
    // Agent 1: Code Analyser
    const codeAnalyserPromptNew =
        "You are a Code Analyser Expert. Your job is to analyze the program and produce a comprehensive report.\n\n" ~
        "## Instructions\n" ~
        "1. Call `getThinkingTemplate(\"code_analysis\")` to get a structured thinking framework.\n" ~
        "2. Follow the template steps to analyze the project thoroughly.\n" ~
        "3. Produce a clear, well-structured report.\n" ~
        "4. Save your report document to the file at `plan/code_analysis.md`.\n" ~
        "5. Add the plan to the RAG with the tool `loadFileToRAG`.\n" ~
        "6. After saving, call 'setPipelineOutput' with 'ok' and call `taskDone` to complete your task.\n";

    const codeAnalyserPromptUpdate =
        "You are a Code Analyser Expert. Your job is to analyze the program and produce a comprehensive report.\n\n" ~
        "## Instructions\n" ~
        "1. A previous code analysis report exist at `plan/code_analysis.md`. Start by reading it one to understand the project, then updated it based on the changed files.\n" ~
        "2. Save your updated report document to the file at `plan/code_analysis.md`.\n" ~
        "3. Add the plan to the RAG with the tool `loadFileToRAG`.\n" ~
        "4. After saving, call 'setPipelineOutput' with 'ok' and call `taskDone` to complete your task.\n";

    // Agent 2: System Designer
    const systemDesignerPrompt =
        "You are a System Designer. Your job is to analyze a user's request and produce a comprehensive system design document.\n\n" ~
        "## Instructions\n" ~
        "1. An analysis of the source code and project is available via the query tools in the first database return from `listRAGDatabases`. Use it when unclear about details in the source code.\n" ~
        "2. Call `getThinkingTemplate(\"system_design\")` to get a structured thinking framework.\n" ~
        "3. Follow the template steps to analyze the user's request thoroughly.\n" ~
        "4. Produce a clear, well-structured system design document.\n" ~
        "5. Save your design document to a file in the `plan/system_design.md`.\n" ~
        "6. After saving, call 'setPipelineOutput' with 'ok' and call `taskDone` to complete your task.\n";

    // Agent 3: System Design reviewer
    // TODO: bullet 3 should reference a system design review template
    const systemDesignerFeedbackPrompt =
        "You are a System Design Reviewer. Your job is to review a system design plan, critique it, and provide actionable feedback for improvement.\n\n" ~
        "## Instructions\n" ~
        "1. Analysis of the source code is available via the query tools in the first database return from `listRAGDatabases`. Use it to better understand the source code.\n" ~
        "2. Read the system design document from `plan/system_design.md` using `readFile`.\n" ~
        "3. Call `getThinkingTemplate(\"system_design\")` to get a structured framework for reviewing designs.\n" ~
        "4. Follow the template to thoroughly analyze the design across dimensions such as requirements clarity, architecture, scalability, reliability, security, cost-efficiency, maintainability, and documentation quality.\n" ~
        "5. Produce a detailed review document that:\n" ~
        "   - Summarizes strengths.\n" ~
        "   - Identifies specific weaknesses, risks, or missing elements.\n" ~
        "   - Provides concrete, actionable suggestions for improvement.\n" ~
        "6. Call 'pipelineOutput' with your review as argument.\n" ~
        "7. After call to 'pipelineOutput', call `taskDone` to complete your task.\n";

    // Agent 4: Implementation Planner
    const implPlannerPrompt =
        "You are an Implementation Planner. Your job is to convert a system design into a\n" ~
        "detailed, actionable implementation plan with individual tasks.\n\n" ~
        "## Instructions\n" ~
        "1. Analysis of the source code is available via the query tools in the first database return from `listRAGDatabases`. Use it to better understand the source code.\n" ~
        "2. Call `getThinkingTemplate(\"implementation_plan\")` to get a structured thinking framework.\n" ~
        "3. Read the system design document from `plan/system_design.md` using `readFile`.\n" ~
        "4. Follow the template steps to break the design into concrete implementation tasks.\n" ~
        "5. Save your implementation plan to `plan/implementation_plan.md` using `writeFile`.\n" ~
        "6. After saving, call 'setPipelineOutput' with 'done' and call `taskDone` to complete your task.\n";
    // dfmt on

    // Create transient agents
    Agent codeAnalyser;
    void initCodeAnalyserAgent() {
        codeAnalyser = new Agent("code_analyser", llmConf, monitor, rag, toolFilter);
        codeAnalyser.setSystemPrompt(
                SystemPromptInit(llmConf.promptToPath(llmConf.agentPrompt)).toString);
    }

    if ((llmConf.workArea ~ "plan/code_analysis.md").exists) {
        if ((Clock.currTime - timeLastModified(
                llmConf.workArea ~ "plan/code_analysis.md")) > 1.dur!"days") {
            // do not update it too often. It is expensive
            // TODO: the time should be configurable
            initCodeAnalyserAgent();
            codeAnalyser.addUserQuery(codeAnalyserPromptUpdate);
        }
    } else {
        initCodeAnalyserAgent();
        codeAnalyser.addUserQuery(codeAnalyserPromptNew);
    }

    auto designer = new Agent("system_designer", llmConf, monitor, rag, toolFilter);
    designer.setSystemPrompt(SystemPromptInit(llmConf.promptToPath(llmConf.agentPrompt)).toString);
    designer.addUserQuery(systemDesignerPrompt);
    designer.addUserQuery(query);

    auto designReview = new Agent("system_design_review", llmConf, monitor, rag, toolFilter);
    designReview.setSystemPrompt(
            SystemPromptInit(llmConf.promptToPath(llmConf.agentPrompt)).toString);
    designReview.addUserQuery(systemDesignerFeedbackPrompt);

    auto planner = new Agent("implementation_planner", llmConf, monitor, rag, toolFilter);
    planner.setSystemPrompt(SystemPromptInit(llmConf.promptToPath(llmConf.agentPrompt)).toString);
    planner.addUserQuery(implPlannerPrompt);

    // Wire into a linear pipeline (no loops)
    // dfmt off
    auto pipelineBuilder = pipelineBuilder
        .addNode("system_design", designer)
        .addNode("system_design_review", designReview)
        .addNode("impl_planner", planner)
        .addEdge("system_design", "system_design_review", null, 1)
        .addEdge("system_design_review", "system_design", null, 0)
        .addEdge("system_design", "impl_planner")
        .stopNode("impl_planner");
    // dfmt on

    if (codeAnalyser is null) {
        pipelineBuilder = pipelineBuilder.startNode("system_design");
    } else {
        pipelineBuilder = pipelineBuilder.addNode("code_analyser", codeAnalyser)
            .addEdge("code_analyser", "system_design").startNode("code_analyser");
    }
    auto pipeline = pipelineBuilder.build();

    logger.trace(pipeline);
    logger.infof("[plan] Starting pipeline for query: %s", query);
    auto result = pipeline.run(interrupt);

    logger.infof("[plan] Pipeline completed: allSuccess=%s, agents=%d",
            result.allSuccess, result.agentResults.length);

    return result;
}
