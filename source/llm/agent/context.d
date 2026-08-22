/// Tool-execution context: `AgentContext` implements the tool interfaces that
/// tool calls operate against, and `VisionImage` carries vision image results.
/// Tool-enabled loops MUST install a taskDone handler via setTaskDoneHandler.
module llm.agent.context;

import core.time : dur;
import logger = std.logger;
import std.algorithm : map, filter;
import std.array : array;
import std.datetime : Clock, SysTime, dur;
import std.path : baseName, stripExtension;
import std.range : empty;
import std.sumtype : match;

import my.optional : Optional;
import my.path : AbsolutePath;

import llm.config : LlmConfig, ToolLimits, RagConfig;
import llm.environment.config : EnvironmentBackend;
import llm.environment.dispatch : EnvironmentContext;
import llm.metric.calculator : MetricsCalculator;
import llm.metric.monitor : MetricMonitor, ToolCallEvent;
import llm.rag.rag : RAG;
import llm.skill : SkillManager;
import llm.tool_call : Context;
import llm.tool_call.completion : CompletionContext;
import llm.tool_call.io : FileContext;
import llm.tool_call.memory : MemoryContext, MemoryTopic;
import llm.tool_call.metrics : MetricsContext;
import llm.tool_call.pipeline : PipelineControlContext;
import llm.tool_call.rag : RAGContext;
import llm.tool_call.skill : SkillContext;
import llm.tool_call.vision : VisionContext, DedicatedVisionAgent, DefaultVisionSystemPrompt;
import llm.types : IAgent;

struct VisionImage {
    string data; // base64 data URL
    string query;
    bool opCast(T)() if (is(T : bool)) {
        return !data.empty;
    }
}

class AgentContext : Context, FileContext, RAGContext, MemoryContext, CompletionContext,
    MetricsContext, PipelineControlContext, VisionContext, SkillContext, EnvironmentContext {
        import llm.vfs : FlatVfs;

        private {
            LlmConfig conf;
            AbsolutePath workArea_;
            RAG rag;
            MetricMonitor monitor;
            MetricsCalculator calculator;
            void delegate(string) taskDoneHandler;
            PipelineControlContext pipelineCtx;
            FlatVfs memoryVfs;

            SysTime nextMetricCalculation;

            VisionImage pendingVisionImage;

            SkillManager skillManager;

            EnvironmentBackend[string] envLookup_;
        }

        /// RAG is optional: when null, RAG-dependent tools degrade gracefully.
        this(LlmConfig conf, RAG rag, MetricMonitor monitor = null) {
            this.conf = conf;
            this.workArea_ = conf.workArea.AbsolutePath;
            this.rag = rag;
            this.monitor = monitor;
            this.memoryVfs = FlatVfs(conf.memoryArea);

            foreach (e; conf.sandboxConfig.executionEnvironments) {
                envLookup_[e.tag] = e;
            }
        }

        /// Set the skill manager used by the skill tool.
        void setSkillManager(SkillManager mgr) {
            skillManager = mgr;
        }

        /// Look up an environment by its tag using a pre-built AA.
        /// Returns the matching backend, or `.init` if not found.
        override EnvironmentBackend getEnvironment(string tag) @safe nothrow {
            auto p = tag in envLookup_;
            return p ? *p : EnvironmentBackend.init;
        }

        /// List all loaded execution environments.
        override EnvironmentBackend[] listEnvironments() @trusted nothrow {
            import std.algorithm : sort;

            return envLookup_.byValue.array.sort!((a, b) => a.tag < b.tag).array;
        }

        /// Get the default environment tag.
        /// Returns: The default tag, or null if not configured.
        override string getDefaultEnvironmentTag() @safe nothrow {
            return conf.sandboxConfig.defaultEnvironmentTag;
        }

        /// Get the default environment tag.
        /// Returns: The default tag, or null if not configured.
        override long getMaxOutputBytes() @safe nothrow {
            return conf.sandboxConfig.maxOutputBytes;
        }

        /// Set the pipeline control context for pipeline output propagation.
        void setPipelineContext(PipelineControlContext ctx) {
            pipelineCtx = ctx;
        }

        /// Install the callback invoked when the taskDone tool is called.
        void setTaskDoneHandler(void delegate(string) handler) {
            taskDoneHandler = handler;
        }

        override SkillManager getSkillManager() {
            return skillManager;
        }

        VisionImage drainVisionImage() nothrow {
            auto result = pendingVisionImage;
            pendingVisionImage = VisionImage.init;
            return result;
        }

        override bool addVisionImage(AbsolutePath path, string query) nothrow {
            try {
                import llm.tool_call.vision : loadImage;

                auto result = loadImage(path);
                if (!result.success)
                    return false;
                pendingVisionImage = VisionImage(result.dataUrl, query);
                return true;
            } catch (Exception e) {
                return false;
            }
        }

        override bool isPathInsideWorkArea(AbsolutePath p) {
            import llm.utility : isPathInsideWorkarea;

            logger.tracef("checking if %s is inside %s", p.toString, workArea_.toString);
            return isPathInsideWorkarea(p, workArea_);
        }

        override AbsolutePath workArea() {
            return workArea_;
        }

        override ToolLimits getToolLimits() {
            return conf.toolLimits;
        }

        override bool hasVisionModel() {
            return !conf.visionModel.isNull;
        }

        override IAgent getVisionAgent() {
            if (conf.visionModel.isNull) {
                throw new Exception("getVisionAgent() called but no vision model is configured");
            }
            auto config = conf.visionModel.get;
            string prompt = config.systemPrompt.length > 0 ? config.systemPrompt
                : DefaultVisionSystemPrompt;
            return new DedicatedVisionAgent(config, prompt);
        }

        override RAG getRAG() {
            return rag;
        }

        override RagConfig getRagConfig() {
            return conf.ragConfig;
        }

        override MemoryTopic[] getMemoryFileTopics() {
            import std.file : timeLastModified;

            try {
                return memoryVfs.getAllFiles.map!(a => MemoryTopic(name: a.baseName.stripExtension, lastModified: timeLastModified(a))).array;
            } catch (Exception e) {
                logger.warning("unable to read memory directories: ", e.msg);
            }
            return null;
        }

        override bool saveMemoryFile(string topic, string content) {
            import llm.rag.rag : add, Document, Origin, Offset;

            auto rval = memoryVfs.save(topic ~ ".md", content);
            if (rval) {
                memoryVfs.query(topic ~ ".md").match!((AbsolutePath a) {
                    if (rag !is null) {
                        rag.add(Document(Origin(a), content, Offset.init), conf.ragConfig);
                    } else {
                        logger.warning("RAG not available, skipping RAG index update");
                    }
                }, (_) {});
            }
            return rval;
        }

        override Optional!string readMemory(string topic) {
            return memoryVfs.read(topic ~ ".md");
        }

        override bool removeMemory(string topic) {
            import llm.rag.rag : Origin;

            memoryVfs.query(topic ~ ".md").match!((AbsolutePath a) {
                if (rag !is null) {
                    rag.removeSource(Origin(a));
                } else {
                    logger.warning("RAG not available, skipping RAG source removal");
                }
            }, (_) {});

            return memoryVfs.remove(topic ~ ".md");
        }

        override ref MetricsCalculator getCalculator() {
            if (Clock.currTime > nextMetricCalculation) {
                if (monitor !is null) // new guard: prevents NPE with null monitor
                    calculator.setEvents(monitor.getRecentEvents(10000));
                nextMetricCalculation = Clock.currTime + 10.dur!"seconds";
            }
            return calculator;
        }

        override ToolCallEvent[] getRecentEvents(long count) {
            if (monitor !is null) {
                return monitor.getRecentEvents(count);
            }
            return null;
        }

        override void taskDone(string answer) {
            if (taskDoneHandler !is null)
                taskDoneHandler(answer);
            else
                logger.warning(
                        "taskDone called but no handler set — tool-enabled loops MUST set one via setTaskDoneHandler");
        }

        override void setPipelineOutput(string output) {
            if (pipelineCtx) {
                pipelineCtx.setPipelineOutput(output);
            } else {
                logger.trace("Pipeline agent produced output but no receiving context set");
            }
        }
    }
