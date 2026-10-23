/// Tool-execution context: `AgentContext` implements the tool interfaces that
/// tool calls operate against, and `VisionImage` carries vision image results.
/// Tool-enabled loops MUST install a taskDone handler via setTaskDoneHandler.
module llm.agent.context;

import core.time : dur;
import logger = std.logger;
import std.algorithm : map, filter;
import std.array : array;
import std.datetime : Clock, SysTime, dur;
import std.exception : collectException;
import std.path : baseName, stripExtension;
import std.range : empty;
import std.sumtype : match;

import my.optional : Optional;
import my.path : AbsolutePath, Path;

import llm.config : LlmConfig, ToolLimits, RagConfig, ToolBrokerConfig,
    memoryShortDir, memoryLongDir;
import llm.environment.config : EnvironmentBackend;
import llm.environment.dispatch : EnvironmentContext;
import llm.metric.calculator : MetricsCalculator;
import llm.metric.monitor : MetricMonitor, ToolCallEvent;
import llm.rag.rag : RAG, Origin;
import llm.rag.dialogue_index : DialogueIndex, DialogueContext;
import llm.rag.reasoning_index : ReasoningIndex, ReasoningContext;
import llm.skill : SkillManager;
import llm.tool_call : Context, RegFunction;
import llm.tool_call.broker : BrokerState;
import llm.tool_call.completion : CompletionContext;
import llm.tool_call.context : ContextManagement;
import llm.tool_call.io : FileContext, FileDirtyTracking;
import llm.tool_call.memory : MemoryContext, MemoryTopic, SaveMemoryResult;
import llm.tool_call.metrics : MetricsContext;
import llm.tool_call.pipeline : PipelineControlContext;
import llm.tool_call.rag : RAGContext;
import llm.tool_call.dialogue;
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

class AgentContext : Context, FileContext, RAGContext, MemoryContext,
    CompletionContext, MetricsContext, PipelineControlContext,
    VisionContext, SkillContext, EnvironmentContext, ContextManagement,
    DialogueContext, ReasoningContext {
        import llm.vfs : FlatVfs;

        mixin FileDirtyTracking;

        private {
            LlmConfig conf;
            AbsolutePath workArea_;
            RAG rag;
            MetricMonitor monitor;
            MetricsCalculator calculator;
            void delegate(string) taskDoneHandler;
            PipelineControlContext pipelineCtx;
            FlatVfs memoryVfs; // read/write view: short/ + long/
            FlatVfs memoryShortVfs;

            SysTime nextMetricCalculation;

            VisionImage pendingVisionImage;

            SkillManager skillManager;

            EnvironmentBackend[string] envLookup_;

            DialogueIndex dialogueIndex;
            ReasoningIndex reasoningIndex;

            bool agentRequstedCompression;
            string agentMessageToSelf;
        }

        // Tool broker state, set by the Agent ctor.
        /// The visible tool pool: the global registry filtered by the per-agent
        /// toolFilter with neverHide tools unioned back, in registry
        /// order.
        RegFunction[] pool;

        /// Per-agent broker state: activated (non-alwaysOn) tool names in
        /// activation order.
        BrokerState broker;

        /// Kill switch: false ⇒ the broker is inert and every tool is
        /// treated as untagged (alwaysOn). Mirrors llmConf.toolBroker.enabled.
        bool brokerEnabled;

        /// Rebuild hook for the model-facing tools array: the Agent
        /// installs a delegate that reassigns its owned `tools` from the pure
        /// selectTools output; the discovery tool invokes it after activating a
        /// tag - the activation change point. Null with the broker
        /// disabled (kill switch): the tools array never changes.
        void delegate() @safe rebuildTools;

        // The resolved tool broker config for the active model. A model switch
        // re-resolves it and applies the per-model adjust hook. Bare contexts
        // hold the struct defaults (broker inert until set).
        ToolBrokerConfig brokerConf;

        /// The owning agent's name (set by the Agent ctor)
        string agentName;

        /// RAG is optional: when null, RAG-dependent tools degrade gracefully.
        this(LlmConfig conf, RAG rag, MetricMonitor monitor = null) {
            this.conf = conf;
            this.workArea_ = conf.workArea.AbsolutePath;
            this.rag = rag;
            this.monitor = monitor;
            this.memoryVfs = FlatVfs(memoryTierDirs(conf.memoryArea));
            this.memoryShortVfs = FlatVfs(conf.memoryArea.map!(a => memoryShortDir(a)).array);

            foreach (e; conf.sandboxConfig.executionEnvironments) {
                envLookup_[e.tag] = e;
            }
        }

        /// Set the skill manager used by the skill tool.
        void setSkillManager(SkillManager mgr) {
            skillManager = mgr;
        }

        bool agentCompressionRequest() @safe nothrow {
            return agentRequstedCompression;
        }

        string agentCompressionMessageToSelf() @safe nothrow {
            return this.agentMessageToSelf;
        }

        void clearAgentCompressionRequest() @safe nothrow {
            agentRequstedCompression = false;
            agentMessageToSelf = null;
        }

        override void agentRequestCompression(string messageToSelf) @safe nothrow {
            agentRequstedCompression = true;
            this.agentMessageToSelf = messageToSelf;
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

        /// Returns: the maximum output bytes for command results.
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

        /// Set the dialogue index (wired by AgentApp after construction).
        void setDialogueIndex(DialogueIndex di) {
            dialogueIndex = di;
        }

        override DialogueIndex getDialogueIndex() {
            return dialogueIndex;
        }

        /// Set the reasoning index (wired by AgentApp after construction).
        void setReasoningIndex(ReasoningIndex ri) {
            reasoningIndex = ri;
        }

        override ReasoningIndex getReasoningIndex() {
            return reasoningIndex;
        }

        override string currentSessionId() {
            return conf.activeChatSessionId;
        }

        override MemoryTopic[] getMemoryFileTopics() {
            import std.file : timeLastModified;

            try {
                return memoryShortVfs.getAllFiles.map!(a => MemoryTopic(name: a.baseName.stripExtension,
                        lastModified: timeLastModified(a))).array;
            } catch (Exception e) {
                logger.warning("unable to read memory directories: ", e.msg);
            }
            return null;
        }

        override SaveMemoryResult saveMemoryFile(string topic, string content) nothrow {
            import std.algorithm : canFind;
            import std.conv : text;
            import std.file : exists, readText, remove;
            import llm.utility : sanitizeUtf8;

            // TODO: this can be slow such as RAG add. An async memory actor
            // should be doing most of the operations in this method.

            try {
                content = content.sanitizeUtf8;

                // the topic exists under a long/ dir => fail, carrying the
                // stored content + guidance; nothing is written. The second
                // write includes the stored content (full content again =
                // replace; stored + additions = merge).
                foreach (area; conf.memoryArea) {
                    auto longFile = memoryLongDir(area) ~ (topic ~ ".md");
                    if (longFile.exists) {
                        auto stored = () {
                            try {
                                return longFile.readText.sanitizeUtf8;
                            } catch (Exception e) {
                                logger.tracef("failed to read file '%s': %s",
                                        longFile, e.msg).collectException;
                            }
                            return null;
                        }();
                        if (!content.canFind(stored))
                            return SaveMemoryResult(false, i"topic '$(topic)' is a long-term memory. Its current content is:$(
                                    stored)

The write above was NOT saved. To replace it, write the full memory content again. To extend it, include the content above plus your additions (the second write decides replace vs merge)."
                                    .text);
                    }
                }

                // Write short/ FIRST, then remove the long/ file + its RAG
                // source (crash-safe order: a crash in between leaves the topic in
                // both tiers, where the next tier pass reconciles it).
                if (!memoryVfs.save(topic ~ ".md", content))
                    return SaveMemoryResult(false, i"error: failed saving the memory with topic '$(
                            topic)'".text);

                // Warn: a missing short/ dir makes the write land long-term
                // (silent promotion).
                memoryVfs.query(topic ~ ".md").match!((AbsolutePath a) {
                    foreach (area; conf.memoryArea)
                        if (a.dirName == AbsolutePath(memoryShortDir(area)))
                            return;
                    logger.tracef("the memory '%s' was written outside a short/ dir (short/ missing?): %s",
                        topic, a);
                }, (_) {});

                if (rag !is null) {
                    foreach (area; conf.memoryArea) {
                        auto longFile = memoryLongDir(area) ~ (topic ~ ".md");
                        if (longFile.exists) {
                            try {
                                rag.db.removeSource(Origin(longFile));
                            } catch (Exception e) {
                                logger.tracef("RAG removal failed: %s", e.msg);
                            }
                            try {
                                remove(longFile);
                            } catch (Exception e) {
                                logger.tracef("failed removing the stale long-term copy of '%s': %s",
                                        topic, e.msg);
                            }
                        }
                    }
                }
            } catch (Exception e) {
                return SaveMemoryResult(false,
                        "error: internal harness error, failed to save the memory: " ~ e.msg);
            }
            return SaveMemoryResult(true, null);
        }

        override Optional!string readMemory(string topic) {
            return memoryVfs.read(topic ~ ".md");
        }

        override bool removeMemory(string topic) {
            import std.file : exists, remove;

            // Remove the RAG source(s) + file from BOTH tiers; archive/ is
            // untouched. Returns true iff the topic existed in a
            // reachable tier, so an archived (or unknown) topic is reported as
            // nonexistent by the removeMemory tool.
            bool removed;
            foreach (area; conf.memoryArea) {
                foreach (dir; [memoryShortDir(area), memoryLongDir(area)]) {
                    auto f = dir ~ (topic ~ ".md");
                    if (!f.exists)
                        continue;
                    removed = true;
                    try {
                        if (rag !is null)
                            rag.db.removeSource(Origin(f));
                    } catch (Exception e) {
                        logger.warningf("RAG source removal failed: %s", e.msg);
                    }
                    try {
                        remove(f);
                    } catch (Exception e) {
                        logger.warningf("failed removing memory '%s': %s", f, e.msg);
                    }
                }
            }
            return removed;
        }

        override ref MetricsCalculator getCalculator() {
            if (Clock.currTime > nextMetricCalculation) {
                if (monitor !is null) // monitor is optional (null by default)
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

        /// The metric monitor (may be null): the JSONL sink for tool-call and
        /// broker events (record / recordEvent).
        MetricMonitor getMetricMonitor() {
            return monitor;
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

/// The read/write VFS hierarchy per memoryArea dir: `short/` first so it
/// shadows `long/` on transient duplicates.
private Path[] memoryTierDirs(Path[] areas) {
    Path[] dirs;
    foreach (area; areas) {
        dirs ~= memoryShortDir(area);
        dirs ~= memoryLongDir(area);
    }
    return dirs;
}

version (unittest) {
    import std.algorithm : canFind;
    import std.conv : to;
    import std.file : exists, mkdirRecurse, readText, write;

    import llm.config : RagDatabaseConfig;
    import llm.rag.rag : Document, Offset, Origin, add;
    import llm.test_util : TestArea, TestEmbedder, testArea;

    /// A context fixture: tier dirs under a private temp area, a real
    /// file-backed RAG over them, and an AgentContext wired to both. The RAG
    /// is registered in the TestArea so cleanup destroys it before the dir is
    /// removed (d2sqlite3's debug build asserts in the GC finalizer otherwise).
    private struct CtxFixture {
        TestArea* testDir;
        Path area;
        LlmConfig conf;
        RAG rag;
        AgentContext ctx;

        this(ref TestArea testDir_) {
            testDir = &testDir_;
            area = testDir_ ~ "memory";
            foreach (sub; ["short", "long", "archive"])
                mkdirRecurse((area ~ sub).toString);

            conf = LlmConfig();
            conf.workArea = testDir_ ~ "ctx";
            mkdirRecurse(conf.workArea.toString);
            conf.memoryArea = [area];
            conf.shortTermDays = 7;
            conf.longTermDays = 100;
            conf.tierPassIntervalHours = 0;

            rag = new RAG(new TestEmbedder(),
                    RagDatabaseConfig(area ~ "rag.sqlite3", "test rag"), null);
            testDir.addRag(rag);
            ctx = new AgentContext(conf, rag);
        }

        /// Write a memory file into the `tier` subdir (short/long/archive).
        void writeMem(string tier, string topic, string content) @safe {
            write((area ~ tier ~ (topic ~ ".md")).toString, content);
        }

        Path memFile(string tier, string topic) @safe pure nothrow {
            return area ~ tier ~ (topic ~ ".md");
        }
    }
}

@("saveMemoryFile on a long-term topic fails with the stored content + guidance; nothing written")
unittest {
    auto testDir = testArea("ctx_f6");
    scope (exit)
        testDir.cleanup();
    auto fx = CtxFixture(testDir);
    immutable stored = "OLD long-term content that is long enough to chunk into several "
        ~ "pieces for the RAG indexer: a b c d e f g h i j k l m n o p q r s t u";
    fx.writeMem("long", "alpha", stored);

    auto r = fx.ctx.saveMemoryFile("alpha",
            "new short-term content that is long enough "
            ~ "to chunk: a b c d e f g h i j k l m n o p q r s t u v w x y z");
    assert(!r.success, "writing over a long-term memory must fail");
    assert(r.message.canFind("is a long-term memory"), r.message);
    assert(r.message.canFind(stored), "the failure must carry the stored content");
    assert(r.message.canFind("write the full memory content again"));
    assert(r.message.canFind("second write decides replace vs merge"));
    assert(readText(fx.memFile("long", "alpha")) == stored, "nothing must be written");
    assert(!fx.memFile("short", "alpha").exists, "the failed write must not land in short/");
}

@(
        "the second write (with the stored content) lands in short/ and removes the long/ file + its RAG source")
unittest {
    auto testDir = testArea("ctx_f7");
    scope (exit)
        testDir.cleanup();
    auto fx = CtxFixture(testDir);
    immutable stored = "OLD long-term content that is long enough to chunk into several "
        ~ "pieces for the RAG indexer: a b c d e f g h i j k l m n o p q r s t u";
    fx.writeMem("long", "alpha", stored);
    fx.rag.add(Document(Origin(fx.memFile("long", "alpha")), stored,
            Offset.init), fx.conf.ragConfig);
    assert(fx.rag.db.getSources.length == 1);

    // The second write carries the stored content plus the additions (merge).
    auto r = fx.ctx.saveMemoryFile("alpha", stored ~ "\n\nplus the new additions that is long "
            ~ "enough to chunk: a b c d e f g h i j k l m n o p q r s t u v w x y z");
    assert(r.success, r.message);
    assert(readText(fx.memFile("short", "alpha")) == stored ~ "\n\nplus the new additions that is long "
            ~ "enough to chunk: a b c d e f g h i j k l m n o p q r s t u v w x y z");
    assert(!fx.memFile("long", "alpha").exists, "the stale long/ file must be removed");
    assert(fx.rag.db.getSources.length == 0, "the long/ file's RAG source must be removed");
}

@("readMemory resolves across both tiers; short shadows long; missing topic is none")
unittest {
    auto testDir = testArea("ctx_f8_read");
    scope (exit)
        testDir.cleanup();
    auto fx = CtxFixture(testDir);
    fx.writeMem("short", "alpha", "SHORT content");
    fx.writeMem("long", "alpha", "LONG content");

    auto read = (string topic) => fx.ctx.readMemory(topic);
    assert(read("alpha").match!((string a) => a == "SHORT content", (_) => false),
            "the short-term copy must shadow the long-term one");
    assert(!read("ghost").match!((string a) => true, (_) => false),
            "a topic in neither tier must read as none");
}

@(
        "removeMemory clears both tiers + RAG sources; archive untouched; archived topic reports nonexistent")
unittest {
    import std.conv : to;

    auto testDir = testArea("ctx_f8_remove");
    scope (exit)
        testDir.cleanup();
    auto fx = CtxFixture(testDir);
    fx.writeMem("short", "alpha", "SHORT content that is long enough: a b c d e f g h");
    fx.writeMem("long", "alpha", "LONG content that is long enough: a b c d e f g h i j");
    fx.rag.add(Document(Origin(fx.memFile("long", "alpha")),
            "LONG content that is long enough: a b c d e f g h i j", Offset.init),
            fx.conf.ragConfig);
    fx.writeMem("archive", "oldie", "COLD content that is unreachable cold storage");
    assert(fx.rag.db.getSources.length == 1);

    assert(fx.ctx.removeMemory("alpha"), "a reachable topic must remove");
    assert(!fx.memFile("short", "alpha").exists && !fx.memFile("long", "alpha").exists);
    assert(fx.rag.db.getSources.length == 0, "both RAG sources must be gone");
    assert(fx.memFile("archive", "oldie").exists, "archive/ is untouched");

    assert(!fx.ctx.removeMemory("oldie"), "an archived topic is unreachable => nonexistent");
    assert(!fx.ctx.removeMemory("ghost"), "an unknown topic is nonexistent");
}

@("getMemoryFileTopics lists short-term only")
unittest {
    auto testDir = testArea("ctx_f10");
    scope (exit)
        testDir.cleanup();
    auto fx = CtxFixture(testDir);
    fx.writeMem("short", "alpha", "SHORT content");
    fx.writeMem("long", "beta", "LONG content");

    auto topics = fx.ctx.getMemoryFileTopics;
    assert(topics.length == 1, "got " ~ topics.length.to!string);
    assert(topics[0].name == "alpha");
}

@("a short-term write does NOT create a RAG entry")
unittest {
    auto testDir = testArea("ctx_o3a");
    scope (exit)
        testDir.cleanup();
    auto fx = CtxFixture(testDir);

    auto r = fx.ctx.saveMemoryFile("fresh", "brand-new short-term content that is long enough "
            ~ "to chunk: a b c d e f g h i j k l m n o p q r s t u v w x y z");
    assert(r.success, r.message);
    assert(readText(fx.memFile("short", "fresh")).canFind("brand-new short-term content"));
    assert(fx.rag.db.getSources.length == 0, "short-term writes are disk-only: no RAG entry");
}
