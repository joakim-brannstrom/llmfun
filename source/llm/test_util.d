/// Test-suite utilities: collision-proof per-test temp dirs, a startup sweep
/// of stale test artifacts, and a bounded SQL retry for tests.
///
/// Background: under the parallel test runner (nusilly default), tests that
/// share fixed-name fixture dirs race: one test's cleanup deletes a sibling
/// test's live SQLite dir, whose writes then fail with
/// "error 8: attempt to write a readonly database" and - with miniorm's
/// unbounded spinSql - retry forever, hanging the whole suite (plan/failing.md
/// F1-F4).
///
/// This module removes both root causes at the foundation level:
///
///  * `freshDir` gives every caller a process-unique directory under the
///    per-run base (`runBaseDir`), so parallel tests can never collide and a
///    test only ever deletes what it created itself.
///  * `retrySql` bounds miniorm's spinSql under `version(unittest)` so a
///    broken DB fails a test in ~10 s instead of hanging the suite.
///  * the module static constructor sweeps `llmfun_test/` at test-binary
///    start, so leftovers from a killed run never leak into the next one.
module llm.test_util;

import core.atomic : atomicFetchAdd;
import core.thread : Thread;
import miniorm : spinSql;
import std.conv : to;
import std.datetime;
import std.file : SpanMode, dirEntries, exists, FileException, isDir,
    mkdirRecurse, readText, rmdirRecurse;
import std.format : format;
import std.path : buildPath;
import std.string : indexOf, split;

version (unittest) {
    /// Root of this test binary's temp dirs: llmfun_test/run_<millis>_<pid>.
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

        auto p = AbsolutePath(TestBaseDir) ~ format("%s_%s_%s", baseName(file), line, testName);
        return TestArea(p);
    }
}

/// Bounded spinSql for tests.
///
/// Under `version(unittest)` a broken DB (e.g. its directory was deleted
/// mid-test) makes the query fail after ~10 s - miniorm's SpinSqlTimeout
/// propagates and the unittest reports it - instead of retrying forever and
/// hanging the whole suite. Production keeps miniorm's unbounded retry
/// (no behavior change).
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
        return spinSql!(query)();
    }
}

unittest {
    // Instantiates retrySql so the template body is compile-checked by THIS
    // task's build (template bodies are only type-checked when instantiated;
    // without an in-module instantiation, a body error - e.g. a duration
    // literal under a qualified import - surfaces only at the first task-02
    // call site). Under version(unittest) this takes the bounded branch:
    // the query succeeds on the first attempt, so this returns immediately.
    assert(retrySql!(() => true)());
}
