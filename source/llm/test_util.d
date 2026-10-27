/// Test-suite utilities: collision-proof per-test temp dirs and a bounded SQL retry for tests.
///
///  * `retrySql` bounds miniorm's spinSql: ~10 s under `version(unittest)`,
///    `llm.rag.database.timeout` (30 s) in production, so a broken DB fails
///    fast instead of hanging the suite or the process.
module llm.test_util;

import core.atomic : atomicFetchAdd;
import core.sys.posix.signal : SIGPIPE, SIG_IGN, signal;
import core.thread : Thread;
import miniorm : spinSql;
import std.conv : to, text;
import std.datetime;
import std.exception : ErrnoException;
import std.file : SpanMode, dirEntries, exists, FileException, isDir,
    mkdirRecurse, readText, rmdirRecurse;
import std.logger : FileLogger, LogLevel, logf, sharedLog;
import std.path : buildPath;
import std.stdio : File, stderr;
import std.string : indexOf, split;

import llm.rag.database : timeout;

version (unittest) {
    import llm.common.embedder : Embedder, EmbedResult;

    /// Deterministic FNV-1a fake embedder (hoisted from memory_tier.d and
    /// agent/context.d, which had diverging copies): equal text -> equal
    /// vector, different text -> different vector.
    package class TestEmbedder : Embedder {
        private int batch;

        this(int batchSize = 50) {
            this.batch = batchSize;
        }

        override string modelName() {
            return "test";
        }

        override long dimensions() {
            return 8;
        }

        override bool supportsTokenization() {
            return false;
        }

        EmbedResult embed(string text) {
            // FNV-1a 64-bit; constants inline, no hash imports needed
            immutable ulong prime = 0x00000100000001B3;
            ulong h = 0xCBF29CE484222325;
            foreach (b; text[])
                h = (h ^ cast(ulong) b) * prime;
            auto vec = new float[8];
            foreach (i; 0 .. 8)
                vec[i] = cast(float)((h >> (8 * i)) & 0xFF) / 255.0;
            return EmbedResult(vec);
        }

        override EmbedResult embedQuery(string text) {
            return embed(text);
        }

        override EmbedResult embedDocument(string text) {
            return embed(text);
        }

        override EmbedResult embedQuery(int[] tokens) {
            return embed(cast(string) tokens);
        }

        override EmbedResult embedDocument(int[] tokens) {
            return embed(cast(string) tokens);
        }

        override int[] tokenize(string text, bool addSpecial) {
            return null;
        }

        override string detokenize(int[] tokens) {
            return null;
        }

        override int batchSize() {
            return batch;
        }

        override void destroy() {
        }
    }

    /// Root of this test binary's temp dirs
    private immutable TestBaseDir = "llmfun_test";

    /// Per-test fixture: the unique directory this unittest owns, created
    /// under the per-process private base (testBaseDir) and removed by
    /// cleanup() on scope(exit). Name is <baseName(file)>_<line>_<testName>,
    /// unique per test call site. For files that share a baseName across the
    /// codebase (e.g. package.d), the caller picks a testName unique among
    /// them.
    struct TestArea {
        import my.path;
        import llm.rag : RAG;

        AbsolutePath workArea;
        RAG[] rags;
        alias workArea this;

        this(Path p) {
            workArea = p.AbsolutePath;
            if (workArea.exists) {
                cleanup();
            }
            mkdirRecurse(workArea);
        }

        /// Register a RAG created inside this area so cleanup destroys it
        /// before the dir is removed: d2sqlite3's debug build asserts in the
        /// GC finalizer (`ensureNotInGC` in database.d), so RAGs must never
        /// be left to the GC.
        void addRag(RAG r) {
            rags ~= r;
        }

        Path createFile(string name, string content = "") @safe {
            auto p = workArea ~ name;
            File(p.toString, "w").write(content);
            return p;
        }

        void cleanup() {
            // guard against removing CWD
            if (workArea == AbsolutePath(".")) {
                return;
            }

            foreach (r; rags) {
                try {
                    r.destroy();
                } catch (Exception) {
                }
            }
            if (exists(workArea)) {
                try {
                    rmdirRecurse(workArea);
                } catch (FileException) {
                }
            }
        }
    }

    /// One unique fixture directory per test, under the private base.
    TestArea testArea(string testName, string file = __FILE__, uint line = __LINE__) {
        import std.path : baseName;
        import my.path;

        auto p = AbsolutePath(TestBaseDir) ~ i"$(baseName(file))_$(line)_$(testName)".text;
        return TestArea(p);
    }

    /// FileLogger that drops the line instead of throwing when the sink
    /// write fails (EAGAIN on a non-draining stderr, EPIPE on a closed pipe).
    class EagainTolerantLogger : FileLogger {
        this() @trusted {
            super(stderr, LogLevel.info);
        }

        override void writeLogMsg(ref LogEntry payload) @trusted {
            try {
                super.writeLogMsg(payload);
            } catch (ErrnoException) {
                // Unhealthy sink - drop the line.
            }
        }
    }
}

/// Bounded spinSql.
///
/// Under `version(unittest)` a broken DB (e.g. its directory was deleted
/// mid-test) makes the query fail after ~10 s - miniorm's SpinSqlTimeout
/// propagates and the unittest reports it - instead of retrying forever and
/// hanging the whole suite. Production is bounded to
/// `llm.rag.database.timeout` (30 s) as well, so an unrecoverable database
/// (e.g. "attempt to write a readonly database" from a full disk) fails fast
/// with SpinSqlTimeout instead of hanging the process.
///
/// Call sites stay byte-identical to the old `spinSql!(lambda)` form: the
/// instantiated zero-arg function is called the same way (D's
/// "function template with all-default arguments" implicit-call rule).
template retrySql(alias query) {
    version (unittest)
        auto retrySql() {
        return spinSql!(query)(10.seconds, 50.msecs, 150.msecs);
    } else
        auto retrySql() {
        return spinSql!(query)(timeout, 50.msecs, 150.msecs);
    }
}

@("instantiating retrySql compile-checks the template body under this module's build")
unittest {
    // Instantiates retrySql so the template body is compile-checked by THIS
    // task's build (template bodies are only type-checked when instantiated;
    // without an in-module instantiation, a body error - e.g. a duration
    // literal under a qualified import - surfaces only at the first real
    // call site). Under version(unittest) this takes the bounded branch:
    // the query succeeds on the first attempt, so this returns immediately.
    assert(retrySql!(() => true)());
}
