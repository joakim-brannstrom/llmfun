/** Native synchronous implementation of the grep_files tool. This module owns
 * the request contract, deterministic candidate traversal, and the streaming
 * matcher used by the public adapter in functions.d. */
module llm.tool_call.io.grep;

import std.algorithm.iteration : splitter;
import std.algorithm.sorting : sort;
import std.array : array, join;
import std.ascii : isAlpha;
import std.conv : text;
import std.file : DirEntry;
import std.range.primitives : empty;
import std.regex : Regex, RegexException, matchFirst, regex;
import std.stdio : File;
import std.string : replace;

import my.path : AbsolutePath;

import llm.config : FilesystemFilterRuntime, PathCategory;
import llm.tool_call : ExecuteFuncResult;
import llm.tool_call.io : FileContext;

// Raw request contract mirroring the GrepFilesParams tool arguments.
struct NativeGrepRequest {
    string path;
    string pattern;
    string glob = "*";
    bool recursive = true;
    bool ignoreCase;
    long maxResults = 20;
}

struct NativeGrepRequestPlan {
    Regex!char pattern;
    string normalizedGlob;
    string displayRoot;
    bool recursive = true;
    long maxResults = 20;
}

struct NativeGrepCompilation {
    NativeGrepRequestPlan plan;
    string error;
    bool valid;
}

NativeGrepCompilation compileNativeGrepRequest(NativeGrepRequest request) @safe {
    NativeGrepRequestPlan plan;
    plan.displayRoot = normalizeNativeGrepDisplayRoot(request.path);
    plan.recursive = request.recursive;
    plan.maxResults = request.maxResults;

    const globCheck = validateNativeGrepGlob(request.glob);
    if (!globCheck.reason.empty) {
        return NativeGrepCompilation(error: i"error: invalid glob '$(request.glob)': $(
                globCheck.reason)".text);
    }
    plan.normalizedGlob = globCheck.normalizedGlob;

    try {
        plan.pattern = regex(request.pattern, request.ignoreCase ? "i" : "");
    } catch (RegexException e) {
        return NativeGrepCompilation(error: i"error: invalid regex '$(request.pattern)': $(e.msg)"
                .text);
    }

    NativeGrepCompilation compiled;
    compiled.plan = plan;
    compiled.valid = true;
    return compiled;
}

string normalizeNativeGrepDisplayRoot(string root) @safe pure {
    string[] segments;
    foreach (segment; replace(root, "\\", "/").splitter('/')) {
        if (segment.empty || segment == ".") {
            continue;
        }
        if (segment == "..") {
            if (segments.length && segments[$ - 1] != "..") {
                segments = segments[0 .. $ - 1];
            } else {
                segments ~= segment;
            }
        } else {
            segments ~= segment;
        }
    }
    if (segments.empty) {
        return ".";
    }
    return segments.join("/");
}

private struct NativeGrepGlobValidation {
    string normalizedGlob;
    string reason;
}

private NativeGrepGlobValidation validateNativeGrepGlob(string glob) @safe pure {
    const converted = replace(glob, "\\", "/");
    const leadingRoot = converted.length && converted[0] == '/';
    const driveQualified = converted.length >= 2 && converted[0].isAlpha && converted[1] == ':';
    if (leadingRoot || driveQualified) {
        return NativeGrepGlobValidation(reason: "must be root-relative");
    }

    string[] segments;
    foreach (segment; converted.splitter('/')) {
        if (segment.empty || segment == ".") {
            continue;
        }
        if (segment == "..") {
            return NativeGrepGlobValidation(reason: "'..' segments are not allowed");
        }
        segments ~= segment;
    }
    const normalized = segments.join("/");

    int brackets;
    int braces;
    foreach (char c; normalized) {
        switch (c) {
        case '[':
            ++brackets;
            break;
        case ']':
            if (brackets == 0) {
                return NativeGrepGlobValidation(reason: "unmatched ']'");
            }
            --brackets;
            break;
        case '{':
            ++braces;
            break;
        case '}':
            if (braces == 0) {
                return NativeGrepGlobValidation(reason: "unmatched '}'");
            }
            --braces;
            break;
        default:
            break;
        }
    }
    if (brackets > 0) {
        return NativeGrepGlobValidation(reason: "unmatched '['");
    }
    if (braces > 0) {
        return NativeGrepGlobValidation(reason: "unmatched '{'");
    }
    return NativeGrepGlobValidation(normalized);
}

@("native_grep glob matcher handles wildcards, hidden files, depth, and brace alternatives")
unittest {
    bool match(string path, string glob) {
        return nativeGrepGlobMatches(path, glob);
    }

    assert(match("parser.d", "*"));
    assert(match("src/nested/leaf.d", "*"));
    assert(match(".hidden.d", "*"));
    assert(match("parser.d", "*.d"));
    assert(match("src/nested/leaf.d", "*.d"));
    assert(match(".hidden.d", "*.d"));
    assert(match(".config/ignored.d", "*.d"));
    assert(!match("src/UPPER.D", "*.d"));
    assert(match("src/types.d", "src/*.d"));
    assert(match("src/nested/leaf.d", "src/*.d"));
    assert(!match("parser.d", "src/*.d"));
    assert(!match(".config/ignored.d", "src/*.d"));
    assert(match("src/types.d", "**/*.d"));
    assert(match(".config/ignored.d", "**/*.d"));
    assert(!match("parser.d", "**/*.d"));
    assert(!match(".hidden.d", "**/*.d"));
    assert(match("src/types.d", "src/*.{d,di}"));
    assert(match("src/nested/leaf.di", "src/*.{d,di}"));
    assert(!match("src/other.txt", "src/*.{d,di}"));
    assert(match("a.d", "[abc]*.d"));
    assert(!match("x.d", "[abc]*.d"));
    assert(match("dx.d", "[!abc]x.d"));
    assert(!match("ax.d", "[!abc]x.d"));
    assert(!match("anything.d", ""));
    assert(!match("", ""));
}

@("native_grep glob normalization maps separators and dot segments before matching")
unittest {
    NativeGrepRequest request;
    request.path = "src";
    request.pattern = "needle";
    request.glob = "src\\*.d";
    const compiled = compileNativeGrepRequest(request);
    assert(compiled.valid, compiled.error);
    assert(compiled.plan.normalizedGlob == "src/*.d", compiled.plan.normalizedGlob);
    assert(nativeGrepGlobMatches("src/x.d", compiled.plan.normalizedGlob));
    assert(!nativeGrepGlobMatches("other/x.d", compiled.plan.normalizedGlob));

    NativeGrepRequest dotted;
    dotted.path = "src";
    dotted.pattern = "needle";
    dotted.glob = "src//./\\*.d";
    const dottedCompiled = compileNativeGrepRequest(dotted);
    assert(dottedCompiled.valid, dottedCompiled.error);
    assert(dottedCompiled.plan.normalizedGlob == "src/*.d", dottedCompiled.plan.normalizedGlob);
}

@("native_grep compile rejects malformed, absolute, and .. globs with fixed reasons")
unittest {
    string globError(string glob) {
        NativeGrepRequest request;
        request.path = "src";
        request.pattern = "needle";
        request.glob = glob;
        const compiled = compileNativeGrepRequest(request);
        assert(!compiled.valid, glob);
        assert(compiled.plan.normalizedGlob.empty);
        return compiled.error;
    }

    assert(globError("/src/*.d") == "error: invalid glob '/src/*.d': must be root-relative");
    assert(globError("C:\\src") == "error: invalid glob 'C:\\src': must be root-relative");
    assert(globError("\\\\srv\\share\\x.d")
            == "error: invalid glob '\\\\srv\\share\\x.d': must be root-relative");
    assert(globError("src/..") == "error: invalid glob 'src/..': '..' segments are not allowed");
    assert(globError("../x") == "error: invalid glob '../x': '..' segments are not allowed");
    assert(globError("a/../b.d") == "error: invalid glob 'a/../b.d': '..' segments are not allowed");
    assert(globError("src/[abc") == "error: invalid glob 'src/[abc': unmatched '['");
    assert(globError("src/{d,di") == "error: invalid glob 'src/{d,di': unmatched '{'");
    assert(globError("src]/*.d") == "error: invalid glob 'src]/*.d': unmatched ']'");
    assert(globError("src}/*.d") == "error: invalid glob 'src}/*.d': unmatched '}'");
}

@("native_grep compile reports glob before regex and embeds the regex message")
unittest {
    import std.algorithm.searching : startsWith;

    NativeGrepRequest request;
    request.path = "src";
    request.glob = "src/[";
    request.pattern = "(unclosed";
    const compiled = compileNativeGrepRequest(request);
    assert(!compiled.valid);
    assert(compiled.error == "error: invalid glob 'src/[': unmatched '['", compiled.error);

    NativeGrepRequest regexOnly;
    regexOnly.path = "src";
    regexOnly.pattern = "(unclosed";
    const regexCompiled = compileNativeGrepRequest(regexOnly);
    assert(!regexCompiled.valid);
    assert(regexCompiled.error.startsWith("error: invalid regex '(unclosed': "),
            regexCompiled.error);
}

@("native_grep compile stores normalized glob, display root, and regex flags")
unittest {
    NativeGrepRequest request;
    request.path = "src//nested/..";
    request.pattern = "needle";
    request.glob = "src/*.d";
    request.ignoreCase = true;
    request.recursive = false;
    request.maxResults = 7;
    const compiled = compileNativeGrepRequest(request);
    assert(compiled.valid, compiled.error);
    assert(compiled.plan.normalizedGlob == "src/*.d");
    assert(compiled.plan.displayRoot == "src");
    assert(compiled.plan.recursive == false);
    assert(compiled.plan.maxResults == 7);
    assert(!matchFirst("NEEDLE found", compiled.plan.pattern).empty);
    assert(matchFirst("nothing here", compiled.plan.pattern).empty);
    assert(!nativeGrepGlobMatches("src/x.d", "*.D"));

    NativeGrepRequest defaults;
    defaults.path = "src";
    defaults.pattern = "needle";
    const defaultCompiled = compileNativeGrepRequest(defaults);
    assert(defaultCompiled.valid, defaultCompiled.error);
    assert(defaultCompiled.plan.normalizedGlob == "*");
    assert(defaultCompiled.plan.displayRoot == "src");
    assert(defaultCompiled.plan.recursive);
    assert(defaultCompiled.plan.maxResults == 20);
    assert(matchFirst("NEEDLE found", defaultCompiled.plan.pattern).empty);
}

@("native_grep display root normalization handles empty roots, separators, and dot segments")
unittest {
    string display(string root) {
        return normalizeNativeGrepDisplayRoot(root);
    }

    assert(display(".") == ".");
    assert(display("./") == ".");
    assert(display("") == ".");
    assert(display("src") == "src");
    assert(display("src/") == "src");
    assert(display("src//") == "src");
    assert(display("./src") == "src");
    assert(display("src/nested/..") == "src");
    assert(display("src//a.d") == "src/a.d");
    assert(display("src//./nested/../x") == "src/x");
    assert(display("a/./b/./c") == "a/b/c");
    assert(display("src\\nested") == "src/nested");
}

@("registered grep_files description exposes function and parameter metadata")
unittest {
    import std.algorithm : canFind, filter;
    import std.array : array;
    import std.traits : getUDAs, hasUDA;

    import llm.tool_call : Function, ParamDescription, ParamOptional,
        descAllFunctions, getFunctions;
    import llm.tool_call.io.functions : GrepFilesParams, grepFiles;

    auto registered = getFunctions().filter!(f => f.nameSnakeCase == "grep_files").array;
    assert(registered.length == 1);
    auto tools = descAllFunctions(registered).array;
    assert(tools.length == 1);
    assert(tools[0]["type"].str == "function");

    auto tool = tools[0]["function"];
    assert(tool["name"].str == "grep_files");
    enum functionDesc = getUDAs!(grepFiles, Function)[0].desc;
    assert(tool["description"].str == functionDesc);

    auto params = tool["parameters"];
    assert(params["type"].str == "object");
    auto properties = params["properties"];
    static foreach (name; __traits(allMembers, GrepFilesParams)) {
        {
            enum descs = getUDAs!(__traits(getMember, GrepFilesParams, name), ParamDescription);
            static if (descs.length)
                assert(properties[name]["description"].str == descs[0].value, name);
            else
                assert("description" !in properties[name], name);

            const required = params["required"].array.canFind!(p => p.str == name);
            static if (hasUDA!(__traits(getMember, GrepFilesParams, name), ParamOptional))
                assert(!required, name);
            else
                assert(required, name);
        }
    }
}

bool nativeGrepGlobMatches(string normalizedRelativePath, string normalizedGlob) @safe pure nothrow {
    import std.path : CaseSensitive, globMatch;

    if (normalizedGlob.empty) {
        return false;
    }
    return globMatch!(CaseSensitive.yes)(normalizedRelativePath, normalizedGlob);
}

// Injectable filesystem operations for deterministic traversal and matching tests.
// Every delegate must be non-null. defaultNativeGrepOps supplies the production set.
struct NativeGrepOps {
    DirEntry[]delegate(string absolutePath) enumerate;
    File delegate(string absolutePath) open;
    size_t delegate(ref File file, ubyte[] buffer) read;
    void delegate(ref File file) close;
    bool delegate(string line, ref Regex!char pattern) matches;
}

NativeGrepOps defaultNativeGrepOps() {
    import std.file : SpanMode, dirEntries;

    NativeGrepOps ops;
    ops.enumerate = (string absolutePath) => dirEntries(absolutePath, SpanMode.shallow).array;
    ops.open = (string absolutePath) => File(absolutePath, "rb");
    ops.read = (ref File file, ubyte[] buffer) => file.rawRead(buffer).length;
    ops.close = (ref File file) { file.close(); };
    ops.matches = (string line, ref Regex!char pattern) => !matchFirst(line, pattern).empty;
    return ops;
}

struct NativeGrepCandidate {
    AbsolutePath absolutePath;
    string relativePath;
    string displayPath;
}

// Outcome of one traversal: a sanitized error, an immediate-stop flag, or neither.
struct NativeGrepTraversal {
    string error;
    bool stopped;
}

alias NativeGrepCandidateSink = bool delegate(ref const NativeGrepCandidate candidate);

private struct NativeGrepBatchEntry {
    string key;
    DirEntry entry;
}

private string nativeGrepRelativeKey(string relativeDir, DirEntry entry) @safe pure {
    import std.path : baseName;

    const child = baseName(entry.name);
    if (relativeDir.empty) {
        return child;
    }
    return relativeDir ~ "/" ~ child;
}

private string nativeGrepJoinDisplay(string displayRoot, string relativePath) @safe pure {
    if (displayRoot == ".") {
        return "./" ~ relativePath;
    }
    return displayRoot ~ "/" ~ relativePath;
}

private string nativeGrepEnumerationError(string displayDir) @safe pure {
    return i"error: directory enumeration failed for '$(displayDir)'".text;
}

private NativeGrepTraversal walkNativeGrepDirectFile(AbsolutePath root, ref NativeGrepRequestPlan plan,
        FilesystemFilterRuntime filter, bool delegate(AbsolutePath) contained,
        NativeGrepCandidateSink sink) {
    if (!contained(root)) {
        return NativeGrepTraversal();
    }
    if (!filter.allows(PathCategory.read, root)) {
        return NativeGrepTraversal();
    }
    if (!nativeGrepGlobMatches(root.baseName, plan.normalizedGlob)) {
        return NativeGrepTraversal();
    }
    auto candidate = NativeGrepCandidate(root, root.baseName, plan.displayRoot);
    if (sink(candidate)) {
        return NativeGrepTraversal(stopped: true);
    }
    return NativeGrepTraversal();
}

private NativeGrepTraversal walkNativeGrepDirectory(AbsolutePath dir,
        string displayDir, string relativeDir, ref NativeGrepRequestPlan plan,
        FilesystemFilterRuntime filter, bool delegate(AbsolutePath) contained,
        ref NativeGrepOps ops, NativeGrepCandidateSink sink) {
    DirEntry[] entries;
    try {
        entries = ops.enumerate(dir.toString);
    } catch (Exception) {
        return NativeGrepTraversal(error: nativeGrepEnumerationError(displayDir));
    }

    NativeGrepBatchEntry[] batch;
    foreach (entry; entries) {
        batch ~= NativeGrepBatchEntry(nativeGrepRelativeKey(relativeDir, entry), entry);
    }
    batch.sort!((a, b) => a.key < b.key);

    foreach (ref item; batch) {
        bool link;
        bool childDirectory;
        bool childFile;
        try {
            link = item.entry.isSymlink;
            if (!link) {
                childDirectory = item.entry.isDir;
                childFile = item.entry.isFile;
            }
        } catch (Exception) {
            return NativeGrepTraversal(error: nativeGrepEnumerationError(displayDir));
        }
        if (link) {
            continue;
        }
        if (childDirectory) {
            if (!plan.recursive) {
                continue;
            }
            auto childAbsolute = AbsolutePath(item.entry.name);
            auto childRelative = item.key;
            auto childDisplay = nativeGrepJoinDisplay(plan.displayRoot, childRelative);
            if (!contained(childAbsolute)) {
                continue;
            }
            auto childResult = walkNativeGrepDirectory(childAbsolute,
                    childDisplay, childRelative, plan, filter, contained, ops, sink);
            if (childResult.stopped || !childResult.error.empty) {
                return childResult;
            }
        } else if (childFile) {
            auto absolute = AbsolutePath(item.entry.name);
            if (!contained(absolute)) {
                continue;
            }
            if (!filter.allows(PathCategory.read, absolute)) {
                continue;
            }
            if (!nativeGrepGlobMatches(item.key, plan.normalizedGlob)) {
                continue;
            }
            auto candidate = NativeGrepCandidate(absolute, item.key,
                    nativeGrepJoinDisplay(plan.displayRoot, item.key));
            if (sink(candidate)) {
                return NativeGrepTraversal(stopped: true);
            }
        }
    }
    return NativeGrepTraversal();
}

version (unittest) {
    private string freshNativeGrepTestDir(string scenario) {
        import llm.test_util : testArea;

        return testArea("native_grep_" ~ scenario).toString;
    }

    private void writeNativeGrepFile(string path, string content) {
        import std.file : mkdirRecurse;
        import std.path : dirName;
        import std.stdio : File;

        mkdirRecurse(dirName(path));
        auto file = File(path, "wb");
        file.write(content);
        file.close();
    }

    private void buildNativeGrepGoldenTree(string base) {
        writeNativeGrepFile(base ~ "/a/0.d", "zero");
        writeNativeGrepFile(base ~ "/a.d", "a");
        writeNativeGrepFile(base ~ "/b.d", "b");
        writeNativeGrepFile(base ~ "/z/c.d", "c");
        writeNativeGrepFile(base ~ "/z0.d", "z0");
    }

    private bool nativeGrepTestContained(AbsolutePath root, AbsolutePath candidate) {
        import std.algorithm.searching : startsWith;

        const w = root.toString;
        const s = candidate.toString;
        return s.startsWith(w) && (s.length == w.length || s[w.length] == '/');
    }

    private NativeGrepRequestPlan nativeGrepTestPlan(NativeGrepRequest request) {
        auto compiled = compileNativeGrepRequest(request);
        assert(compiled.valid, compiled.error);
        return compiled.plan;
    }
}

@("native_grep traversal returns deterministic candidate order for a mixed tree")
unittest {
    import std.file : rmdirRecurse;

    const base = freshNativeGrepTestDir("golden_order");
    scope (exit)
        rmdirRecurse(base);

    buildNativeGrepGoldenTree(base);

    auto root = AbsolutePath(base);
    NativeGrepRequest request;
    request.path = ".";
    request.pattern = "anything";
    auto plan = nativeGrepTestPlan(request);

    FilesystemFilterRuntime filter;
    bool contained(AbsolutePath p) {
        return nativeGrepTestContained(root, p);
    }

    string[] seen;
    bool sink(ref const NativeGrepCandidate candidate) {
        seen ~= candidate.relativePath ~ "|" ~ candidate.displayPath;
        return false;
    }

    auto ops = defaultNativeGrepOps();
    const golden = [
        "a/0.d|./a/0.d", "a.d|./a.d", "b.d|./b.d", "z/c.d|./z/c.d", "z0.d|./z0.d"
    ];

    const first = walkNativeGrepCandidates(root, true, plan, filter, &contained, ops, &sink);
    assert(first.error.empty && !first.stopped, first.error);
    assert(seen == golden, seen.join(","));

    seen = [];
    const second = walkNativeGrepCandidates(root, true, plan, filter, &contained, ops, &sink);
    assert(second.error.empty && !second.stopped);
    assert(seen == golden, seen.join(","));
}

@("native_grep sibling ordering is case-sensitive ordinal")
unittest {
    import std.file : rmdirRecurse;

    const base = freshNativeGrepTestDir("case_ordinal");
    scope (exit)
        rmdirRecurse(base);

    writeNativeGrepFile(base ~ "/a.d", "a");
    writeNativeGrepFile(base ~ "/B.d", "B");

    auto root = AbsolutePath(base);
    NativeGrepRequest request;
    request.path = ".";
    request.pattern = "anything";
    auto plan = nativeGrepTestPlan(request);

    FilesystemFilterRuntime filter;
    bool contained(AbsolutePath p) {
        return nativeGrepTestContained(root, p);
    }

    string[] seen;
    bool sink(ref const NativeGrepCandidate candidate) {
        seen ~= candidate.relativePath;
        return false;
    }

    auto ops = defaultNativeGrepOps();
    const result = walkNativeGrepCandidates(root, true, plan, filter, &contained, ops, &sink);
    assert(result.error.empty && !result.stopped, result.error);
    assert(seen == ["B.d", "a.d"], seen.join(","));
}

@("native_grep recursive false emits only immediate regular-file children")
unittest {
    import std.file : rmdirRecurse;

    const base = freshNativeGrepTestDir("flat_only");
    scope (exit)
        rmdirRecurse(base);

    buildNativeGrepGoldenTree(base);

    auto root = AbsolutePath(base);
    NativeGrepRequest request;
    request.path = ".";
    request.pattern = "anything";
    request.recursive = false;
    auto plan = nativeGrepTestPlan(request);

    FilesystemFilterRuntime filter;
    bool contained(AbsolutePath p) {
        return nativeGrepTestContained(root, p);
    }

    string[] seen;
    bool sink(ref const NativeGrepCandidate candidate) {
        seen ~= candidate.relativePath;
        return false;
    }

    auto ops = defaultNativeGrepOps();
    const result = walkNativeGrepCandidates(root, true, plan, filter, &contained, ops, &sink);
    assert(result.error.empty && !result.stopped, result.error);
    assert(seen == ["a.d", "b.d", "z0.d"], seen.join(","));
}

@("native_grep glob mismatch on a directory never prunes descent into it")
unittest {
    import std.file : rmdirRecurse;

    const base = freshNativeGrepTestDir("no_prune");
    scope (exit)
        rmdirRecurse(base);

    writeNativeGrepFile(base ~ "/sub/leaf.d", "leaf");
    writeNativeGrepFile(base ~ "/top.d", "top");

    auto root = AbsolutePath(base);
    NativeGrepRequest request;
    request.path = ".";
    request.pattern = "anything";
    request.glob = "*.d";
    auto plan = nativeGrepTestPlan(request);

    FilesystemFilterRuntime filter;
    bool contained(AbsolutePath p) {
        return nativeGrepTestContained(root, p);
    }

    string[] seen;
    bool sink(ref const NativeGrepCandidate candidate) {
        seen ~= candidate.relativePath;
        return false;
    }

    auto ops = defaultNativeGrepOps();
    const result = walkNativeGrepCandidates(root, true, plan, filter, &contained, ops, &sink);
    assert(result.error.empty && !result.stopped, result.error);
    assert(seen == ["sub/leaf.d", "top.d"], seen.join(","));
}

@("native_grep prefixed globs match deeper descendants through separators")
unittest {
    import std.file : rmdirRecurse;

    const base = freshNativeGrepTestDir("nested_glob");
    scope (exit)
        rmdirRecurse(base);

    writeNativeGrepFile(base ~ "/parser.d", "parser");
    writeNativeGrepFile(base ~ "/src/x.d", "x");
    writeNativeGrepFile(base ~ "/src/nested/leaf.d", "leaf");

    auto root = AbsolutePath(base);
    NativeGrepRequest request;
    request.path = ".";
    request.pattern = "anything";
    request.glob = "src/*.d";
    auto plan = nativeGrepTestPlan(request);

    FilesystemFilterRuntime filter;
    bool contained(AbsolutePath p) {
        return nativeGrepTestContained(root, p);
    }

    string[] seen;
    bool sink(ref const NativeGrepCandidate candidate) {
        seen ~= candidate.relativePath;
        return false;
    }

    auto ops = defaultNativeGrepOps();
    const result = walkNativeGrepCandidates(root, true, plan, filter, &contained, ops, &sink);
    assert(result.error.empty && !result.stopped, result.error);
    assert(seen == ["src/nested/leaf.d", "src/x.d"], seen.join(","));
}

@("native_grep brace globs select alternatives across depth")
unittest {
    import std.file : rmdirRecurse;

    const base = freshNativeGrepTestDir("brace_glob");
    scope (exit)
        rmdirRecurse(base);

    writeNativeGrepFile(base ~ "/a.d", "a");
    writeNativeGrepFile(base ~ "/b.di", "b");
    writeNativeGrepFile(base ~ "/c.txt", "c");
    writeNativeGrepFile(base ~ "/sub/d.di", "d");

    auto root = AbsolutePath(base);
    NativeGrepRequest request;
    request.path = ".";
    request.pattern = "anything";
    request.glob = "*.{d,di}";
    auto plan = nativeGrepTestPlan(request);

    FilesystemFilterRuntime filter;
    bool contained(AbsolutePath p) {
        return nativeGrepTestContained(root, p);
    }

    string[] seen;
    bool sink(ref const NativeGrepCandidate candidate) {
        seen ~= candidate.relativePath;
        return false;
    }

    auto ops = defaultNativeGrepOps();
    const result = walkNativeGrepCandidates(root, true, plan, filter, &contained, ops, &sink);
    assert(result.error.empty && !result.stopped, result.error);
    assert(seen == ["a.d", "b.di", "sub/d.di"], seen.join(","));
}

@("native_grep empty glob selects nothing without errors")
unittest {
    import std.file : rmdirRecurse;

    const base = freshNativeGrepTestDir("empty_glob");
    scope (exit)
        rmdirRecurse(base);

    writeNativeGrepFile(base ~ "/a.d", "a");
    writeNativeGrepFile(base ~ "/sub/b.d", "b");

    auto root = AbsolutePath(base);
    NativeGrepRequest request;
    request.path = ".";
    request.pattern = "anything";
    request.glob = "";
    auto plan = nativeGrepTestPlan(request);

    FilesystemFilterRuntime filter;
    bool contained(AbsolutePath p) {
        return nativeGrepTestContained(root, p);
    }

    string[] seen;
    bool sink(ref const NativeGrepCandidate candidate) {
        seen ~= candidate.relativePath;
        return false;
    }

    auto ops = defaultNativeGrepOps();
    const result = walkNativeGrepCandidates(root, true, plan, filter, &contained, ops, &sink);
    assert(result.error.empty && !result.stopped, result.error);
    assert(seen.empty, seen.join(","));
}

@("native_grep traversal skips symlink entries including dangling ones")
unittest {
    import std.file : rmdirRecurse, symlink;

    const base = freshNativeGrepTestDir("symlinks");
    scope (exit)
        rmdirRecurse(base);

    writeNativeGrepFile(base ~ "/real.d", "real");
    symlink(base ~ "/real.d", base ~ "/link.d");
    symlink(base ~ "/missing.d", base ~ "/broken.d");

    auto root = AbsolutePath(base);
    NativeGrepRequest request;
    request.path = ".";
    request.pattern = "anything";
    auto plan = nativeGrepTestPlan(request);

    FilesystemFilterRuntime filter;
    bool contained(AbsolutePath p) {
        return nativeGrepTestContained(root, p);
    }

    string[] seen;
    bool sink(ref const NativeGrepCandidate candidate) {
        seen ~= candidate.relativePath;
        return false;
    }

    auto ops = defaultNativeGrepOps();
    const result = walkNativeGrepCandidates(root, true, plan, filter, &contained, ops, &sink);
    assert(result.error.empty && !result.stopped, result.error);
    assert(seen == ["real.d"], seen.join(","));
}

@("native_grep direct file root selects by base name and uses the file argument as display")
unittest {
    import std.file : rmdirRecurse;

    const base = freshNativeGrepTestDir("direct_file");
    scope (exit)
        rmdirRecurse(base);

    writeNativeGrepFile(base ~ "/sub/w.d", "content");

    auto root = AbsolutePath(base ~ "/sub/w.d");
    NativeGrepRequest request;
    request.path = "sub//w.d";
    request.pattern = "content";
    auto plan = nativeGrepTestPlan(request);
    assert(plan.displayRoot == "sub/w.d", plan.displayRoot);

    auto defaults = defaultNativeGrepOps();
    int enumerations;
    int opens;
    NativeGrepOps spy;
    spy.enumerate = (string p) { ++enumerations; return defaults.enumerate(p); };
    spy.open = (string p) { ++opens; return defaults.open(p); };
    spy.read = (ref File f, ubyte[] b) => defaults.read(f, b);
    spy.close = (ref File f) { defaults.close(f); };
    spy.matches = (string line, ref Regex!char pattern) => defaults.matches(line, pattern);

    FilesystemFilterRuntime filter;
    bool contained(AbsolutePath p) {
        return nativeGrepTestContained(AbsolutePath(base), p);
    }

    string[] seen;
    bool sink(ref const NativeGrepCandidate candidate) {
        seen ~= candidate.relativePath ~ "|" ~ candidate.displayPath;
        return false;
    }

    const result = walkNativeGrepCandidates(root, false, plan, filter, &contained, spy, &sink);
    assert(result.error.empty && !result.stopped, result.error);
    assert(seen == ["w.d|sub/w.d"], seen.join(","));
    assert(enumerations == 0, enumerations.text);
    assert(opens == 0, opens.text);

    NativeGrepRequest flat;
    flat.path = "sub//w.d";
    flat.pattern = "content";
    flat.recursive = false;
    auto flatPlan = nativeGrepTestPlan(flat);
    seen = [];
    const flatResult = walkNativeGrepCandidates(root, false, flatPlan, filter,
            &contained, spy, &sink);
    assert(flatResult.error.empty && !flatResult.stopped);
    assert(seen == ["w.d|sub/w.d"], seen.join(","));

    NativeGrepRequest prefixed;
    prefixed.path = "sub//w.d";
    prefixed.pattern = "content";
    prefixed.glob = "src/*.d";
    auto prefixedPlan = nativeGrepTestPlan(prefixed);
    seen = [];
    const prefixedResult = walkNativeGrepCandidates(root, false, prefixedPlan,
            filter, &contained, spy, &sink);
    assert(prefixedResult.error.empty && !prefixedResult.stopped);
    assert(seen.empty, seen.join(","));
    assert(opens == 0, opens.text);

    NativeGrepRequest deep;
    deep.path = "sub//w.d";
    deep.pattern = "content";
    deep.glob = "**/*.d";
    auto deepPlan = nativeGrepTestPlan(deep);
    seen = [];
    walkNativeGrepCandidates(root, false, deepPlan, filter, &contained, spy, &sink);
    assert(seen.empty, seen.join(","));
    assert(opens == 0, opens.text);
}

@("native_grep traversal enumerates each visited directory once and stops without later batches")
unittest {
    import std.algorithm.searching : canFind;
    import std.file : rmdirRecurse;

    const base = freshNativeGrepTestDir("spy_stop");
    scope (exit)
        rmdirRecurse(base);

    buildNativeGrepGoldenTree(base);

    auto root = AbsolutePath(base);
    NativeGrepRequest request;
    request.path = ".";
    request.pattern = "anything";
    auto plan = nativeGrepTestPlan(request);

    auto defaults = defaultNativeGrepOps();
    int enumerations;
    string[] enumerated;
    int opens;
    NativeGrepOps spy;
    spy.enumerate = (string p) {
        ++enumerations;
        enumerated ~= p;
        return defaults.enumerate(p);
    };
    spy.open = (string p) { ++opens; return defaults.open(p); };
    spy.read = (ref File f, ubyte[] b) => defaults.read(f, b);
    spy.close = (ref File f) { defaults.close(f); };
    spy.matches = (string line, ref Regex!char pattern) => defaults.matches(line, pattern);

    FilesystemFilterRuntime filter;
    bool contained(AbsolutePath p) {
        return nativeGrepTestContained(root, p);
    }

    bool sink(ref const NativeGrepCandidate candidate) {
        return true;
    }

    const result = walkNativeGrepCandidates(root, true, plan, filter, &contained, spy, &sink);
    assert(result.stopped && result.error.empty, result.error);
    assert(enumerations == 2, enumerated.join(","));
    assert(enumerated[0] == root.toString, enumerated.join(","));
    assert(enumerated[1] == root.toString ~ "/a", enumerated.join(","));
    assert(!enumerated.canFind(root.toString ~ "/z"), enumerated.join(","));
    assert(opens == 0, opens.text);
}

NativeGrepTraversal walkNativeGrepCandidates(AbsolutePath root, bool rootIsDirectory,
        ref NativeGrepRequestPlan plan, FilesystemFilterRuntime filter,
        bool delegate(AbsolutePath) contained, ref NativeGrepOps ops, NativeGrepCandidateSink sink) {
    if (!rootIsDirectory) {
        return walkNativeGrepDirectFile(root, plan, filter, contained, sink);
    }
    return walkNativeGrepDirectory(root, plan.displayRoot, string.init, plan,
            filter, contained, ops, sink);
}

version (unittest) {
    private FilesystemFilterRuntime nativeGrepTestFilter(string[] include, string[] exclude) {
        import my.filter : ReFilter;

        FilesystemFilterRuntime filter;
        filter.readFilter = ReFilter(include, exclude);
        filter.enabled = true;
        return filter;
    }
}

@("native_grep traversal never prunes children with the parent directory filter verdict")
unittest {
    import std.file : rmdirRecurse;

    const base = freshNativeGrepTestDir("deny_parent");
    scope (exit)
        rmdirRecurse(base);

    writeNativeGrepFile(base ~ "/denied/allowed.d", "allowed");
    writeNativeGrepFile(base ~ "/denied/inner.d", "inner");
    writeNativeGrepFile(base ~ "/ok.d", "ok");

    auto root = AbsolutePath(base);
    NativeGrepRequest request;
    request.path = ".";
    request.pattern = "anything";
    auto plan = nativeGrepTestPlan(request);

    auto defaults = defaultNativeGrepOps();
    string[] enumerated;
    NativeGrepOps spy;
    spy.enumerate = (string p) { enumerated ~= p; return defaults.enumerate(p); };
    spy.open = (string p) { return defaults.open(p); };
    spy.read = (ref File f, ubyte[] b) => defaults.read(f, b);
    spy.close = (ref File f) { defaults.close(f); };
    spy.matches = (string line, ref Regex!char pattern) => defaults.matches(line, pattern);

    bool contained(AbsolutePath p) {
        return nativeGrepTestContained(root, p);
    }

    string[] seen;
    bool sink(ref const NativeGrepCandidate candidate) {
        seen ~= candidate.displayPath;
        return false;
    }

    const deniedPath = AbsolutePath(base ~ "/denied").toString;

    // A denied parent is still enumerated, and each child is judged independently.
    seen = [];
    const parentDenied = walkNativeGrepCandidates(root, true, plan,
            nativeGrepTestFilter([], [deniedPath ~ "$"]), &contained, spy, &sink);
    assert(parentDenied.error.empty && !parentDenied.stopped, parentDenied.error);
    assert(seen == ["./denied/allowed.d", "./denied/inner.d", "./ok.d"], seen.join(","));
    assert(enumerated.length == 2, enumerated.join(","));
    assert(enumerated[1] == root.toString ~ "/denied", enumerated.join(","));

    // File denials do not cascade or prevent enumeration of the parent directory.
    seen = [];
    enumerated = [];
    const childrenDenied = walkNativeGrepCandidates(root, true, plan,
            nativeGrepTestFilter([], [deniedPath ~ "/.*"]), &contained, spy, &sink);
    assert(childrenDenied.error.empty && !childrenDenied.stopped, childrenDenied.error);
    assert(seen == ["./ok.d"], seen.join(","));
    assert(enumerated.length == 2, enumerated.join(","));
}

@("native_grep traversal leaves denied malformed subtree unopened")
unittest {
    import std.file : rmdirRecurse;

    const base = freshNativeGrepTestDir("denied_bytes");
    scope (exit)
        rmdirRecurse(base);

    // The denied subtree produces no candidates.
    // Its malformed names and contents must remain untouched.
    writeNativeGrepFile(base ~ "/denied/bad.d", "\xFF\xFE garbage after a matchable prefix");
    writeNativeGrepFile(base ~ "/denied/no-read.d", "unreadable-looking content");
    writeNativeGrepFile(base ~ "/ok.d", "ok");

    auto root = AbsolutePath(base);
    NativeGrepRequest request;
    request.path = ".";
    request.pattern = "anything";
    auto plan = nativeGrepTestPlan(request);

    auto defaults = defaultNativeGrepOps();
    int opens;
    NativeGrepOps spy;
    spy.enumerate = (string p) { return defaults.enumerate(p); };
    spy.open = (string p) { ++opens; return File(); };
    spy.read = (ref File f, ubyte[] b) => defaults.read(f, b);
    spy.close = (ref File f) { defaults.close(f); };
    spy.matches = (string line, ref Regex!char pattern) => defaults.matches(line, pattern);

    bool contained(AbsolutePath p) {
        return nativeGrepTestContained(root, p);
    }

    string[] seen;
    bool sink(ref const NativeGrepCandidate candidate) {
        seen ~= candidate.displayPath;
        return false;
    }

    const deniedPath = AbsolutePath(base ~ "/denied").toString;
    const result = walkNativeGrepCandidates(root, true, plan,
            nativeGrepTestFilter([], [deniedPath ~ "/.*"]), &contained, spy, &sink);

    assert(result.error.empty && !result.stopped, result.error);
    assert(seen == ["./ok.d"], seen.join(","));
    assert(opens == 0, opens.text);
}

@("native_grep traversal skips nested symlinks without touching their targets")
unittest {
    import std.algorithm.searching : canFind;
    import std.file : mkdir, readText, rmdirRecurse, symlink;

    const base = freshNativeGrepTestDir("nested_links");
    const outside = base ~ "_outside";
    scope (exit) {
        rmdirRecurse(base);
        rmdirRecurse(outside);
    }

    writeNativeGrepFile(outside ~ "/canary.txt", "keep");
    writeNativeGrepFile(base ~ "/real.d", "real");
    mkdir(base ~ "/sub");
    symlink(outside ~ "/canary.txt", base ~ "/sub/link.d");
    symlink(outside, base ~ "/linkdir");

    auto root = AbsolutePath(base);
    NativeGrepRequest request;
    request.path = ".";
    request.pattern = "anything";
    auto plan = nativeGrepTestPlan(request);

    auto defaults = defaultNativeGrepOps();
    string[] enumerated;
    int opens;
    NativeGrepOps spy;
    spy.enumerate = (string p) { enumerated ~= p; return defaults.enumerate(p); };
    spy.open = (string p) { ++opens; return File(); };
    spy.read = (ref File f, ubyte[] b) => defaults.read(f, b);
    spy.close = (ref File f) { defaults.close(f); };
    spy.matches = (string line, ref Regex!char pattern) => defaults.matches(line, pattern);

    bool contained(AbsolutePath p) {
        return nativeGrepTestContained(root, p);
    }

    string[] seen;
    bool sink(ref const NativeGrepCandidate candidate) {
        seen ~= candidate.displayPath;
        return false;
    }

    const result = walkNativeGrepCandidates(root, true, plan,
            FilesystemFilterRuntime(), &contained, spy, &sink);

    assert(result.error.empty && !result.stopped, result.error);
    assert(seen == ["./real.d"], seen.join(","));
    assert(opens == 0, opens.text);
    assert(enumerated.length == 2, enumerated.join(","));
    assert(!enumerated.canFind(AbsolutePath(outside).toString), enumerated.join(","));
    assert(readText(outside ~ "/canary.txt") == "keep");
}

version (Posix) {
    @("native_grep traversal ignores fifo and socket special entries")
    unittest {
        import core.sys.posix.sys.stat : mkfifo;
        import core.sys.posix.sys.types : mode_t;
        import std.file : rmdirRecurse;
        import std.socket : AddressFamily, Socket, SocketType, UnixAddress;
        import std.string : toStringz;

        const base = freshNativeGrepTestDir("specials");
        scope (exit)
            rmdirRecurse(base);

        writeNativeGrepFile(base ~ "/real.d", "real");
        mkfifo((base ~ "/fifo").toStringz, cast(mode_t) 384);
        auto socket = new Socket(AddressFamily.UNIX, SocketType.STREAM);
        socket.bind(new UnixAddress(base ~ "/sock"));
        socket.close();

        auto root = AbsolutePath(base);
        NativeGrepRequest request;
        request.path = ".";
        request.pattern = "anything";
        auto plan = nativeGrepTestPlan(request);

        auto defaults = defaultNativeGrepOps();
        int opens;
        NativeGrepOps spy;
        spy.enumerate = (string p) { return defaults.enumerate(p); };
        // Opening a FIFO could block, so the spy asserts that no entry is opened.
        spy.open = (string p) { ++opens; return File(); };
        spy.read = (ref File f, ubyte[] b) => defaults.read(f, b);
        spy.close = (ref File f) { defaults.close(f); };
        spy.matches = (string line, ref Regex!char pattern) => defaults.matches(line, pattern);

        bool contained(AbsolutePath p) {
            return nativeGrepTestContained(root, p);
        }

        string[] seen;
        bool sink(ref const NativeGrepCandidate candidate) {
            seen ~= candidate.displayPath;
            return false;
        }

        const result = walkNativeGrepCandidates(root, true, plan,
                FilesystemFilterRuntime(), &contained, spy, &sink);

        assert(result.error.empty && !result.stopped, result.error);
        assert(seen == ["./real.d"], seen.join(","));
        assert(opens == 0, opens.text);
    }
}

@("native_grep traversal reports caller-relative enumeration failures")
unittest {
    import std.algorithm.searching : endsWith;
    import std.file : rmdirRecurse;

    const base = freshNativeGrepTestDir("enumerate_error");
    scope (exit)
        rmdirRecurse(base);

    writeNativeGrepFile(base ~ "/denied/leaf.d", "leaf");
    writeNativeGrepFile(base ~ "/ok.d", "ok");

    auto root = AbsolutePath(base);
    NativeGrepRequest request;
    request.path = ".";
    request.pattern = "anything";
    auto plan = nativeGrepTestPlan(request);

    auto defaults = defaultNativeGrepOps();
    NativeGrepOps spy;
    spy.enumerate = (string p) {
        if (p.endsWith("/denied")) {
            throw new Exception("secret /absolute/root detail");
        }
        return defaults.enumerate(p);
    };
    spy.open = (string p) { return defaults.open(p); };
    spy.read = (ref File f, ubyte[] b) => defaults.read(f, b);
    spy.close = (ref File f) { defaults.close(f); };
    spy.matches = (string line, ref Regex!char pattern) => defaults.matches(line, pattern);

    bool contained(AbsolutePath p) {
        return nativeGrepTestContained(root, p);
    }

    string[] seen;
    bool sink(ref const NativeGrepCandidate candidate) {
        seen ~= candidate.displayPath;
        return false;
    }

    // Baseline without a directory filter.
    const plain = walkNativeGrepCandidates(root, true, plan,
            FilesystemFilterRuntime(), &contained, spy, &sink);
    assert(plain.error == "error: directory enumeration failed for './denied'", plain.error);
    assert(!plain.stopped);
    assert(seen.empty, seen.join(","));

    // A denied directory still reports the enumeration error.
    // Descent does not consult the filter.
    const deniedPath = AbsolutePath(base ~ "/denied").toString;
    const wouldDeny = walkNativeGrepCandidates(root, true, plan,
            nativeGrepTestFilter([], [deniedPath ~ "$"]), &contained, spy, &sink);
    assert(wouldDeny.error == "error: directory enumeration failed for './denied'", wouldDeny.error);
    assert(!wouldDeny.stopped);
    assert(seen.empty, seen.join(","));
}

@("native_grep traversal silently skips denied files, mismatches, nested links, and specials")
unittest {
    import std.algorithm.searching : canFind;
    import std.file : readText, rmdirRecurse, symlink;

    const base = freshNativeGrepTestDir("mixed_skip");
    const outside = base ~ "_outside";
    scope (exit) {
        rmdirRecurse(base);
        rmdirRecurse(outside);
    }

    writeNativeGrepFile(base ~ "/a/0.d", "zero");
    writeNativeGrepFile(base ~ "/a.d", "a");
    writeNativeGrepFile(base ~ "/b.d", "b");
    writeNativeGrepFile(base ~ "/denied/x.d", "x");
    writeNativeGrepFile(base ~ "/notes.txt", "notes");
    writeNativeGrepFile(base ~ "/z/c.d", "c");
    writeNativeGrepFile(base ~ "/z0.d", "z0");

    writeNativeGrepFile(outside ~ "/canary.txt", "keep");
    symlink(outside ~ "/canary.txt", base ~ "/z/link.d");

    version (Posix) {
        import core.sys.posix.sys.stat : mkfifo;
        import core.sys.posix.sys.types : mode_t;
        import std.string : toStringz;

        mkfifo((base ~ "/z/fifo").toStringz, cast(mode_t) 384);
    }

    auto root = AbsolutePath(base);
    NativeGrepRequest request;
    request.path = ".";
    request.pattern = "anything";
    request.glob = "*.d";
    auto plan = nativeGrepTestPlan(request);

    auto defaults = defaultNativeGrepOps();
    string[] enumerated;
    int opens;
    NativeGrepOps spy;
    spy.enumerate = (string p) { enumerated ~= p; return defaults.enumerate(p); };
    spy.open = (string p) { ++opens; return File(); };
    spy.read = (ref File f, ubyte[] b) => defaults.read(f, b);
    spy.close = (ref File f) { defaults.close(f); };
    spy.matches = (string line, ref Regex!char pattern) => defaults.matches(line, pattern);

    bool contained(AbsolutePath p) {
        return nativeGrepTestContained(root, p);
    }

    string[] seen;
    bool sink(ref const NativeGrepCandidate candidate) {
        seen ~= candidate.displayPath;
        return false;
    }

    // The unfiltered baseline includes allowed regular files in merged sorted order.
    const unfiltered = walkNativeGrepCandidates(root, true, plan,
            FilesystemFilterRuntime(), &contained, spy, &sink);
    assert(unfiltered.error.empty && !unfiltered.stopped, unfiltered.error);
    assert(seen == [
        "./a/0.d", "./a.d", "./b.d", "./denied/x.d", "./z/c.d", "./z0.d"
    ], seen.join(","));
    assert(opens == 0, opens.text);
    assert(enumerated.canFind(root.toString ~ "/denied"), enumerated.join(","));

    // Denied files, mismatches, links, and special entries are silent skips.
    // Remaining descendants preserve their paths and baseline order.
    seen = [];
    enumerated = [];
    const deniedPath = AbsolutePath(base ~ "/denied").toString;
    const filtered = walkNativeGrepCandidates(root, true, plan, nativeGrepTestFilter([
    ], ["/b\\.d$", "/denied/", "/denied$"]), &contained, spy, &sink);
    assert(filtered.error.empty && !filtered.stopped, filtered.error);
    assert(seen == ["./a/0.d", "./a.d", "./z/c.d", "./z0.d"], seen.join(","));
    assert(opens == 0, opens.text);
    assert(enumerated.canFind(root.toString ~ "/denied"), enumerated.join(","));
    assert(readText(outside ~ "/canary.txt") == "keep");
}

struct NativeGrepMatch {
    string displayPath;
    long lineNumber;
    string text;
}

struct NativeGrepScan {
    string error;
    bool stopped;
}

alias NativeGrepMatchSink = bool delegate(ref const NativeGrepMatch match);

private string nativeGrepUtf8Error(const(char)[] displayPath, long lineNumber) @safe pure {
    return i"error: invalid UTF-8 in '$(displayPath)' at line $(lineNumber)".text;
}

private string nativeGrepRegexError(const(char)[] displayPath, long lineNumber) @safe pure {
    return i"error: regex evaluation failed in '$(displayPath)' at line $(lineNumber)".text;
}

NativeGrepScan evaluateNativeGrepLine(const(ubyte)[] line, long lineNumber,
        ref const NativeGrepCandidate candidate, ref Regex!char pattern,
        ref NativeGrepOps ops, NativeGrepMatchSink sink) {
    import std.utf : UTFException, validate;

    // Validate the whole line before matching so malformed trailing bytes cannot hide behind a valid prefix.
    // A plain cast lets validate report invalid input instead of triggering the assumeUTF debug assertion.
    string lineText = cast(string) line;
    try {
        validate(lineText);
    } catch (UTFException) {
        return NativeGrepScan(error: nativeGrepUtf8Error(candidate.displayPath, lineNumber));
    }

    bool matched;
    try {
        matched = ops.matches(lineText, pattern);
    } catch (Exception) {
        return NativeGrepScan(error: nativeGrepRegexError(candidate.displayPath, lineNumber));
    }
    if (!matched) {
        return NativeGrepScan();
    }

    // Match text owns its bytes because the framer reuses its pending buffer and read chunks.
    auto match = NativeGrepMatch(cast(string) candidate.displayPath, lineNumber,
            cast(string) line.idup);
    if (sink(match)) {
        return NativeGrepScan(stopped: true);
    }
    return NativeGrepScan();
}

private ptrdiff_t nativeGrepFindLf(const(ubyte)[] bytes) {
    foreach (i, b; bytes) {
        if (b == 0x0A) {
            return cast(ptrdiff_t) i;
        }
    }
    return -1;
}

struct NativeGrepLineFramer {
    private NativeGrepScan delegate(const(ubyte)[] line, long lineNumber) onLine;
    private ubyte[] pending;
    private long nextLine = 1;

    this(NativeGrepScan delegate(const(ubyte)[] line, long lineNumber) onLine) {
        this.onLine = onLine;
    }

    NativeGrepScan feed(const(ubyte)[] chunk) {
        size_t position;
        while (true) {
            const lf = nativeGrepFindLf(chunk[position .. $]);
            if (lf < 0) {
                break;
            }
            const lineEnd = position + cast(size_t) lf;
            const(ubyte)[] lineBytes;
            if (pending.empty) {
                lineBytes = chunk[position .. lineEnd];
            } else {
                lineBytes = pending;
                pending.length = 0;
                if (lineEnd > position) {
                    lineBytes ~= chunk[position .. lineEnd];
                }
            }
            position = lineEnd + 1;
            // Remove one CR before a consumed LF. Preserve a lone CR on an unterminated final line.
            if (!lineBytes.empty && lineBytes[$ - 1] == 0x0D) {
                lineBytes = lineBytes[0 .. $ - 1];
            }
            const scan = onLine(lineBytes, nextLine++);
            if (!scan.error.empty || scan.stopped) {
                return scan;
            }
        }
        if (position < chunk.length) {
            pending ~= chunk[position .. $];
        }
        return NativeGrepScan();
    }

    NativeGrepScan finish() {
        if (pending.empty) {
            return NativeGrepScan();
        }
        const lineBytes = pending;
        pending = [];
        return onLine(lineBytes, nextLine++);
    }
}

version (unittest) {
    private void nativeGrepAssertFraming(string input, string[] expected) {
        import std.conv : text;

        const bytes = cast(const(ubyte)[]) input;

        // Whole input in a single chunk.
        {
            string[] seen;
            auto framer = NativeGrepLineFramer((const(ubyte)[] line, long number) {
                seen ~= number.text ~ "|" ~ cast(string) line.dup;
                return NativeGrepScan();
            });
            const scan = framer.feed(bytes);
            assert(scan.error.empty && !scan.stopped, scan.error);
            const done = framer.finish();
            assert(done.error.empty && !done.stopped, done.error);
            assert(seen == expected, text(seen));
        }

        // One byte per chunk.
        {
            string[] seen;
            auto framer = NativeGrepLineFramer((const(ubyte)[] line, long number) {
                seen ~= number.text ~ "|" ~ cast(string) line.dup;
                return NativeGrepScan();
            });
            foreach (i; 0 .. bytes.length) {
                const scan = framer.feed(bytes[i .. i + 1]);
                assert(scan.error.empty && !scan.stopped, scan.error);
            }
            const done = framer.finish();
            assert(done.error.empty && !done.stopped, done.error);
            assert(seen == expected, text(seen));
        }

        // Every possible two-chunk split, including empty half-chunks.
        foreach (split; 0 .. bytes.length + 1) {
            string[] seen;
            auto framer = NativeGrepLineFramer((const(ubyte)[] line, long number) {
                seen ~= number.text ~ "|" ~ cast(string) line.dup;
                return NativeGrepScan();
            });
            const first = framer.feed(bytes[0 .. split]);
            assert(first.error.empty && !first.stopped, first.error);
            const second = framer.feed(bytes[split .. $]);
            assert(second.error.empty && !second.stopped, second.error);
            const done = framer.finish();
            assert(done.error.empty && !done.stopped, done.error);
            assert(seen == expected, text(seen));
        }
    }
}

@("native_grep line framer handles whole, byte-wise, and split chunks identically")
unittest {
    nativeGrepAssertFraming("", []);
    nativeGrepAssertFraming("a", ["1|a"]);
    nativeGrepAssertFraming("a\n", ["1|a"]);
    nativeGrepAssertFraming("\n", ["1|"]);
    nativeGrepAssertFraming("a\n\n", ["1|a", "2|"]);
    nativeGrepAssertFraming("a\n\nb", ["1|a", "2|", "3|b"]);
    nativeGrepAssertFraming("a\r\n", ["1|a"]);
    // Exactly one CR is stripped because a greedy strip would drop the second.
    nativeGrepAssertFraming("a\r\r\n", ["1|a\r"]);
    nativeGrepAssertFraming("a\r", ["1|a\r"]);
}

@("native_grep line evaluator matches valid lines, owns text, and honors sink stops")
unittest {
    import std.conv : text;

    auto pattern = regex("needle");
    auto candidate = NativeGrepCandidate(AbsolutePath("/irrelevant/x.d"), "x.d", "sub/x.d");
    auto ops = defaultNativeGrepOps();
    int matcherCalls;
    ops.matches = (string line, ref Regex!char p) { ++matcherCalls; return true; };

    string[] retained;
    bool sink(ref const NativeGrepMatch match) {
        retained ~= match.displayPath ~ "|" ~ match.text;
        return match.lineNumber == 2;
    }

    // Whitespace, colons, and a final lone CR survive verbatim in the
    // owned record text.
    const matchScan = evaluateNativeGrepLine(cast(const(ubyte)[]) "  needle: 12\r",
            2, candidate, pattern, ops, &sink);
    assert(matchScan.stopped && matchScan.error.empty, matchScan.error);
    assert(retained == ["sub/x.d|  needle: 12\r"], retained.text);
    assert(matcherCalls == 1, matcherCalls.text);
}

@("native_grep line evaluator validates UTF-8 before any matcher call")
unittest {
    auto pattern = regex("matchme");
    auto candidate = NativeGrepCandidate(AbsolutePath("/irrelevant/x.d"), "x.d", "sub/x.d");
    auto ops = defaultNativeGrepOps();
    int matcherCalls;
    ops.matches = (string line, ref Regex!char p) { ++matcherCalls; return true; };

    bool sink(ref const NativeGrepMatch match) {
        return false;
    }

    // The matchable prefix must not mask the malformed trailing bytes.
    const scan = evaluateNativeGrepLine(cast(const(ubyte)[]) "matchme\xFF tail",
            7, candidate, pattern, ops, &sink);

    assert(scan.error == "error: invalid UTF-8 in 'sub/x.d' at line 7", scan.error);
    assert(!scan.stopped);
    assert(matcherCalls == 0, matcherCalls.text);
}

@("native_grep line evaluator reports sanitized regex evaluation failures")
unittest {
    auto pattern = regex("needle");
    auto candidate = NativeGrepCandidate(AbsolutePath("/irrelevant/x.d"), "x.d", "sub/x.d");
    auto ops = defaultNativeGrepOps();
    int matcherCalls;
    ops.matches = (string line, ref Regex!char p) {
        ++matcherCalls;
        throw new Exception("secret /absolute/root detail");
    };

    bool sink(ref const NativeGrepMatch match) {
        return false;
    }

    const scan = evaluateNativeGrepLine(cast(const(ubyte)[]) "needle", 3,
            candidate, pattern, ops, &sink);

    assert(scan.error == "error: regex evaluation failed in 'sub/x.d' at line 3", scan.error);
    assert(!scan.stopped);
    assert(matcherCalls == 1, matcherCalls.text);
}

@("native_grep line evaluator invokes the matcher once per valid line")
unittest {
    import std.conv : text;

    auto candidate = NativeGrepCandidate(AbsolutePath("/irrelevant/x.d"), "x.d", "sub/x.d");
    auto ops = defaultNativeGrepOps();
    int matcherCalls;
    ops.matches = (string line, ref Regex!char p) { ++matcherCalls; return true; };

    string[] seen;
    bool sink(ref const NativeGrepMatch match) {
        seen ~= match.lineNumber.text ~ "|" ~ match.text;
        return false;
    }

    // Repeated regex hits on one line still produce exactly one record.
    auto anyA = regex("a");
    const repeated = evaluateNativeGrepLine(cast(const(ubyte)[]) "aa", 4,
            candidate, anyA, ops, &sink);
    assert(repeated.error.empty && !repeated.stopped, repeated.error);
    assert(matcherCalls == 1, matcherCalls.text);
    assert(seen == ["4|aa"], seen.join(","));

    // Empty lines reach the matcher once. A `^$` pattern produces one empty-text record.
    auto atLineStartEnd = regex("^$");
    const(ubyte)[] emptyLine;
    const emptyResult = evaluateNativeGrepLine(emptyLine, 5, candidate,
            atLineStartEnd, ops, &sink);
    assert(emptyResult.error.empty && !emptyResult.stopped, emptyResult.error);
    assert(matcherCalls == 2, matcherCalls.text);
    assert(seen == ["4|aa", "5|"], seen.join(","));
}

@("native_grep line framer sink stop prevents later matcher calls in the same chunk")
unittest {
    auto pattern = regex("needle");
    auto candidate = NativeGrepCandidate(AbsolutePath("/irrelevant/x.d"), "x.d", "sub/x.d");
    auto ops = defaultNativeGrepOps();
    int matcherCalls;
    ops.matches = (string line, ref Regex!char p) { ++matcherCalls; return true; };

    string[] seen;
    auto framer = NativeGrepLineFramer((const(ubyte)[] line, long number) {
        return evaluateNativeGrepLine(line, number, candidate, pattern, ops,
            (ref const NativeGrepMatch match) { seen ~= match.text; return true; });
    });

    const scan = framer.feed(cast(const(ubyte)[]) "needle\nmore\nneedle-later\n");
    assert(scan.stopped && scan.error.empty, scan.error);
    assert(seen == ["needle"], seen.join(","));
    assert(matcherCalls == 1, matcherCalls.text);
}

@("native_grep line framer and evaluator match multibyte UTF-8 split across chunks")
unittest {
    auto pattern = regex("h.llo");
    auto candidate = NativeGrepCandidate(AbsolutePath("/irrelevant/x.d"), "x.d", "sub/x.d");
    auto ops = defaultNativeGrepOps();

    string[] seen;
    auto framer = NativeGrepLineFramer((const(ubyte)[] line, long number) {
        return evaluateNativeGrepLine(line, number, candidate, pattern, ops,
            (ref const NativeGrepMatch match) {
            seen ~= match.lineNumber.text ~ "|" ~ match.displayPath ~ "|" ~ match.text;
            return false;
        });
    });

    // Split inside the two-byte e-acute sequence, not at a character boundary.
    const bytes = cast(const(ubyte)[]) "h\xc3\xa9llo\n";
    const first = framer.feed(bytes[0 .. 2]);
    assert(first.error.empty && !first.stopped, first.error);
    const second = framer.feed(bytes[2 .. $]);
    assert(second.error.empty && !second.stopped, second.error);
    const done = framer.finish();
    assert(done.error.empty && !done.stopped, done.error);

    assert(seen == ["1|sub/x.d|h\xc3\xa9llo"], seen.join(","));
}

@("native_grep line evaluator owns match text across framer buffer reuse")
unittest {
    auto pattern = regex("needle");
    auto candidate = NativeGrepCandidate(AbsolutePath("/irrelevant/x.d"), "x.d", "sub/x.d");
    auto ops = defaultNativeGrepOps();

    string[] retained;
    auto framer = NativeGrepLineFramer((const(ubyte)[] line, long number) {
        return evaluateNativeGrepLine(line, number, candidate, pattern, ops,
            (ref const NativeGrepMatch match) {
            retained ~= match.text;
            return false;
        });
    });

    // Small chunks reuse the pending buffer between matching lines.
    // Retained match text must remain unchanged.
    const bytes = cast(const(ubyte)[]) "needle-one\nfiller\nneedle-two\nfiller\n";
    size_t offset;
    while (offset < bytes.length) {
        const end = offset + 3 < bytes.length ? offset + 3 : bytes.length;
        const scan = framer.feed(bytes[offset .. end]);
        assert(scan.error.empty && !scan.stopped, scan.error);
        offset = end;
    }
    const done = framer.finish();
    assert(done.error.empty && !done.stopped, done.error);

    assert(retained == ["needle-one", "needle-two"], retained.join(","));
}

@("native_grep line framer propagates evaluator errors without later lines")
unittest {
    auto pattern = regex("matchme");
    auto candidate = NativeGrepCandidate(AbsolutePath("/irrelevant/x.d"), "x.d", "sub/x.d");
    auto ops = defaultNativeGrepOps();
    int matcherCalls;
    ops.matches = (string line, ref Regex!char p) { ++matcherCalls; return true; };

    string[] seen;
    auto framer = NativeGrepLineFramer((const(ubyte)[] line, long number) {
        return evaluateNativeGrepLine(line, number, candidate, pattern, ops,
            (ref const NativeGrepMatch match) { seen ~= match.text; return false; });
    });

    // Stop immediately on error, leaving later lines unexamined and finish uncalled.
    const scan = framer.feed(cast(const(ubyte)[]) "matchme\xFF tail\nlater\n");
    assert(scan.error == "error: invalid UTF-8 in 'sub/x.d' at line 1", scan.error);
    assert(!scan.stopped);
    assert(seen.empty, seen.join(","));
    assert(matcherCalls == 0, matcherCalls.text);
}

// Close failures never replace the selected scan result.
private void nativeGrepGuardedClose(ref NativeGrepOps ops, ref File file) {
    try {
        ops.close(file);
    } catch (Exception) {
    }
}

enum NativeGrepBinaryPrefix = 8192;

private string nativeGrepOpenError(const(char)[] displayPath) @safe pure {
    return i"error: file open failed for '$(displayPath)'".text;
}

private string nativeGrepReadError(const(char)[] displayPath) @safe pure {
    return i"error: file read failed for '$(displayPath)'".text;
}

// Scans one candidate through one content handle and closes it through nativeGrepGuardedClose.
// Check the first 8192 bytes for NUL before matching.
// Feed a clean prefix to the framer once, then continue reading the same handle without losing or duplicating bytes.
NativeGrepScan scanNativeGrepFile(ref const NativeGrepCandidate candidate,
        ref Regex!char pattern, ref NativeGrepOps ops, NativeGrepMatchSink sink) {
    File file;
    try {
        file = ops.open(candidate.absolutePath.toString);
    } catch (Exception) {
        return NativeGrepScan(error: nativeGrepOpenError(candidate.displayPath));
    }

    scope (exit)
        nativeGrepGuardedClose(ops, file);

    ubyte[NativeGrepBinaryPrefix] buffer;
    size_t prefixLength;
    bool eof;
    while (prefixLength < buffer.length) {
        size_t got;
        try {
            got = ops.read(file, buffer[prefixLength .. $]);
        } catch (Exception) {
            return NativeGrepScan(error: nativeGrepReadError(candidate.displayPath));
        }
        if (got == 0) {
            eof = true;
            break;
        }
        prefixLength += got;
    }
    const prefix = buffer[0 .. prefixLength];

    // A NUL in offsets 0..8191 skips the file before matching.
    // A NUL at offset 8192 remains ordinary line data.
    foreach (b; prefix) {
        if (b == 0) {
            return NativeGrepScan();
        }
    }

    auto framer = NativeGrepLineFramer((const(ubyte)[] line, long number) {
        return evaluateNativeGrepLine(line, number, candidate, pattern, ops, sink);
    });

    // The prefix boundary need not align with a line or UTF-8 sequence.
    // Feed the collected bytes once and continue the same stream with later reads.
    const prefixScan = framer.feed(prefix);
    if (!prefixScan.error.empty || prefixScan.stopped) {
        return prefixScan;
    }

    while (!eof) {
        size_t got;
        try {
            got = ops.read(file, buffer[]);
        } catch (Exception) {
            return NativeGrepScan(error: nativeGrepReadError(candidate.displayPath));
        }
        if (got == 0) {
            break;
        }
        const scan = framer.feed(buffer[0 .. got]);
        if (!scan.error.empty || scan.stopped) {
            return scan;
        }
    }

    // Ordinary EOF frames a nonempty unterminated final line once.
    // Empty input and a terminal LF add no phantom line.
    return framer.finish();
}

private enum NativeGrepRootKind {
    regularFile,
    directory,
    unsupported
}

private string nativeGrepUnsupportedRootError(const(char)[] displayRoot) @safe pure {
    return i"error: path '$(displayRoot)': regular file or directory required".text;
}

// Classify the validated root without following symlinks.
// Report stat failures and entries that change after validation as unsupported roots.
// The sanitized error excludes low-level filesystem text.
private NativeGrepRootKind classifyNativeGrepRoot(AbsolutePath root) {
    DirEntry entry;
    try {
        entry = DirEntry(root.toString);
    } catch (Exception) {
        return NativeGrepRootKind.unsupported;
    }
    try {
        if (entry.isSymlink) {
            return NativeGrepRootKind.unsupported;
        }
        if (entry.isDir) {
            return NativeGrepRootKind.directory;
        }
        if (entry.isFile) {
            return NativeGrepRootKind.regularFile;
        }
    } catch (Exception) {
        return NativeGrepRootKind.unsupported;
    }
    return NativeGrepRootKind.unsupported;
}

// Render retained records as `path:line:text`, joined by LF without a trailing LF.
private string nativeGrepRenderMatches(const(NativeGrepMatch)[] records) @safe pure {
    string[] rendered;
    foreach (record; records) {
        rendered ~= record.displayPath ~ ":" ~ text(record.lineNumber) ~ ":" ~ record.text;
    }
    return rendered.join("\n");
}

/** Runs native grep with production filesystem operations.
Params:
    ctx, root, plan = live file context, validated absolute root, and compiled request.
Returns: LF-joined `path:line:text` matches or the first sanitized failure.
Empty success means no matches. Cap stops keep matches. Failures discard them. */
ExecuteFuncResult runNativeGrep(ref FileContext ctx, AbsolutePath root, NativeGrepRequestPlan plan) {
    auto ops = defaultNativeGrepOps();
    return runNativeGrepWithOps(ctx, root, plan, ops);
}

/** Runs native grep with injectable operations for test spies.
Params:
    ops, ctx, root, plan = complete delegate set, live context, validated root, and compiled request.
Returns: the same result contract as runNativeGrep.
All filesystem calls use ops. */
ExecuteFuncResult runNativeGrepWithOps(ref FileContext ctx, AbsolutePath root,
        NativeGrepRequestPlan plan, ref NativeGrepOps ops) {
    // maxResults is validated before compilation. A non-positive value here is a programmer error.
    // Fail fast so an invalid cap cannot disable the result limit in release builds.
    if (plan.maxResults < 1) {
        throw new Error("grep_files plan requires maxResults >= 1");
    }

    const kind = classifyNativeGrepRoot(root);
    if (kind == NativeGrepRootKind.unsupported) {
        return ExecuteFuncResult(nativeGrepUnsupportedRootError(plan.displayRoot), success: false);
    }

    NativeGrepMatch[] records;
    long emittedCount;
    string scanError;

    bool matchSink(ref const NativeGrepMatch match) {
        records ~= match;
        ++emittedCount;
        assert(emittedCount >= 0 && emittedCount <= plan.maxResults);
        return emittedCount == plan.maxResults;
    }

    bool candidateSink(ref const NativeGrepCandidate candidate) {
        const scan = scanNativeGrepFile(candidate, plan.pattern, ops, &matchSink);
        if (!scan.error.empty) {
            // Save scan errors separately so they can replace every accumulated match.
            scanError = scan.error;
            return true;
        }
        // Propagate the cap stop as the walker's stop signal.
        // The current shallow batch is the only permitted look-ahead.
        return scan.stopped;
    }

    bool contained(AbsolutePath candidate) {
        return ctx.isPathInsideWorkArea(candidate);
    }

    const traversal = walkNativeGrepCandidates(root, kind == NativeGrepRootKind.directory, plan,
            ctx.getToolLimits().filesystemFilterRuntime, &contained, ops, &candidateSink);
    if (!traversal.error.empty) {
        return ExecuteFuncResult(traversal.error, success: false);
    }
    if (!scanError.empty) {
        return ExecuteFuncResult(scanError, success: false);
    }
    if (records.empty) {
        return ExecuteFuncResult("", success: true);
    }
    return ExecuteFuncResult(nativeGrepRenderMatches(records), success: true);
}

version (unittest) {
    private struct NativeGrepSpyCounters {
        int opens, closes, readCalls, matcherCalls;
        size_t bytesRead;
    }

    // Count each default operation per call. Tests may replace individual delegates for failure injection.
    private NativeGrepOps nativeGrepCountingSpy(ref NativeGrepOps defaults,
            ref NativeGrepSpyCounters counters) {
        NativeGrepOps spy;
        spy.enumerate = (string p) => defaults.enumerate(p);
        spy.open = (string p) { ++counters.opens; return defaults.open(p); };
        spy.read = (ref File f, ubyte[] b) {
            ++counters.readCalls;
            const got = defaults.read(f, b);
            counters.bytesRead += got;
            return got;
        };
        spy.close = (ref File f) { ++counters.closes; defaults.close(f); };
        spy.matches = (string line, ref Regex!char pattern) {
            ++counters.matcherCalls;
            return defaults.matches(line, pattern);
        };
        return spy;
    }

    private NativeGrepCandidate nativeGrepScanCandidate(string base, string name) {
        return NativeGrepCandidate(AbsolutePath(base ~ "/" ~ name), name, "./" ~ name);
    }
}

@("native_grep scanner opens and closes once per file with exact byte continuity")
unittest {
    import std.file : rmdirRecurse;

    const base = freshNativeGrepTestDir("scan_continuity");
    scope (exit)
        rmdirRecurse(base);

    const content = "needle\nfiller\nmore needle\n";
    writeNativeGrepFile(base ~ "/x.d", content);
    auto candidate = nativeGrepScanCandidate(base, "x.d");
    auto pattern = regex("needle");

    auto defaults = defaultNativeGrepOps();
    auto counters = NativeGrepSpyCounters();
    auto spy = nativeGrepCountingSpy(defaults, counters);

    string[] records;
    bool sink(ref const NativeGrepMatch match) {
        records ~= match.lineNumber.text ~ "|" ~ match.displayPath ~ "|" ~ match.text;
        return false;
    }

    const scan = scanNativeGrepFile(candidate, pattern, spy, &sink);

    assert(scan.error.empty && !scan.stopped, scan.error);
    assert(counters.opens == 1, counters.opens.text);
    assert(counters.closes == 1, counters.closes.text);
    assert(counters.readCalls == 2, counters.readCalls.text);
    assert(counters.bytesRead == content.length, counters.bytesRead.text);
    assert(counters.matcherCalls == 3, counters.matcherCalls.text);
    assert(records == ["1|./x.d|needle", "3|./x.d|more needle"], records.join(","));
}

@("native_grep scanner keeps framing and continuity across one-byte reads")
unittest {
    import std.file : rmdirRecurse;

    const base = freshNativeGrepTestDir("scan_short_reads");
    scope (exit)
        rmdirRecurse(base);

    const content = "ab\ncd\n";
    writeNativeGrepFile(base ~ "/x.d", content);
    auto candidate = nativeGrepScanCandidate(base, "x.d");
    auto pattern = regex("c");

    auto defaults = defaultNativeGrepOps();
    auto counters = NativeGrepSpyCounters();
    auto spy = nativeGrepCountingSpy(defaults, counters);
    spy.read = (ref File f, ubyte[] b) {
        ++counters.readCalls;
        if (b.length > 1) {
            b = b[0 .. 1];
        }
        const got = defaults.read(f, b);
        counters.bytesRead += got;
        return got;
    };

    string[] records;
    bool sink(ref const NativeGrepMatch match) {
        records ~= match.lineNumber.text ~ "|" ~ match.text;
        return false;
    }

    const scan = scanNativeGrepFile(candidate, pattern, spy, &sink);

    assert(scan.error.empty && !scan.stopped, scan.error);
    assert(counters.opens == 1 && counters.closes == 1,
            counters.opens.text ~ "/" ~ counters.closes.text);
    assert(counters.readCalls == content.length + 1, counters.readCalls.text);
    assert(counters.bytesRead == content.length, counters.bytesRead.text);
    assert(counters.matcherCalls == 2, counters.matcherCalls.text);
    assert(records == ["2|cd"], records.join(","));
}

@("native_grep scanner cap stop performs no additional read or matcher call")
unittest {
    import std.array : replicate;
    import std.file : rmdirRecurse;

    const base = freshNativeGrepTestDir("scan_cap_stop");
    scope (exit)
        rmdirRecurse(base);

    const content = "needle\n".replicate(2000);
    writeNativeGrepFile(base ~ "/x.d", content);
    auto candidate = nativeGrepScanCandidate(base, "x.d");
    auto pattern = regex("needle");

    auto defaults = defaultNativeGrepOps();
    auto counters = NativeGrepSpyCounters();
    auto spy = nativeGrepCountingSpy(defaults, counters);

    string[] records;
    bool sink(ref const NativeGrepMatch match) {
        records ~= match.lineNumber.text ~ "|" ~ match.text;
        return true;
    }

    const scan = scanNativeGrepFile(candidate, pattern, spy, &sink);

    assert(scan.stopped && scan.error.empty, scan.error);
    assert(records == ["1|needle"], records.join(","));
    assert(counters.matcherCalls == 1, counters.matcherCalls.text);
    assert(counters.readCalls == 1, counters.readCalls.text);
    assert(counters.bytesRead == 8192, counters.bytesRead.text);
    assert(counters.opens == 1 && counters.closes == 1,
            counters.opens.text ~ "/" ~ counters.closes.text);
}

@("native_grep scanner skips NUL at offset 0 without any matcher call")
unittest {
    import std.file : rmdirRecurse;

    const base = freshNativeGrepTestDir("scan_nul_zero");
    scope (exit)
        rmdirRecurse(base);

    const content = "needle\n\x00rest";
    writeNativeGrepFile(base ~ "/x.d", content);
    auto candidate = nativeGrepScanCandidate(base, "x.d");
    auto pattern = regex("needle");

    auto defaults = defaultNativeGrepOps();
    auto counters = NativeGrepSpyCounters();
    auto spy = nativeGrepCountingSpy(defaults, counters);

    string[] records;
    bool sink(ref const NativeGrepMatch match) {
        records ~= match.text;
        return false;
    }

    const scan = scanNativeGrepFile(candidate, pattern, spy, &sink);

    assert(scan.error.empty && !scan.stopped, scan.error);
    assert(records.empty, records.join(","));
    assert(counters.matcherCalls == 0, counters.matcherCalls.text);
    assert(counters.opens == 1 && counters.closes == 1,
            counters.opens.text ~ "/" ~ counters.closes.text);
    assert(counters.readCalls == 2, counters.readCalls.text);
    assert(counters.bytesRead == content.length, counters.bytesRead.text);
}

@("native_grep scanner NUL at offset 8191 skips and at 8192 does not")
unittest {
    import std.array : replicate;
    import std.file : rmdirRecurse;

    const base = freshNativeGrepTestDir("scan_nul_boundary");
    scope (exit)
        rmdirRecurse(base);

    writeNativeGrepFile(base ~ "/inside.d", "a".replicate(8191) ~ "\x00needle\n");
    writeNativeGrepFile(base ~ "/outside.d", "a".replicate(8192) ~ "\x00needle\n");
    auto pattern = regex("needle");
    auto defaults = defaultNativeGrepOps();

    auto insideCounters = NativeGrepSpyCounters();
    auto insideSpy = nativeGrepCountingSpy(defaults, insideCounters);
    auto insideCandidate = nativeGrepScanCandidate(base, "inside.d");
    string[] insideRecords;
    bool insideSink(ref const NativeGrepMatch match) {
        insideRecords ~= match.text;
        return false;
    }

    const insideScan = scanNativeGrepFile(insideCandidate, pattern, insideSpy, &insideSink);
    assert(insideScan.error.empty && !insideScan.stopped, insideScan.error);
    assert(insideRecords.empty, insideRecords.join(","));
    assert(insideCounters.matcherCalls == 0, insideCounters.matcherCalls.text);
    assert(insideCounters.opens == 1 && insideCounters.closes == 1,
            insideCounters.opens.text ~ "/" ~ insideCounters.closes.text);
    assert(insideCounters.readCalls == 1, insideCounters.readCalls.text);
    assert(insideCounters.bytesRead == 8192, insideCounters.bytesRead.text);

    auto outsideCounters = NativeGrepSpyCounters();
    auto outsideSpy = nativeGrepCountingSpy(defaults, outsideCounters);
    auto outsideCandidate = nativeGrepScanCandidate(base, "outside.d");
    string[] outsideRecords;
    bool outsideSink(ref const NativeGrepMatch match) {
        outsideRecords ~= match.text;
        return false;
    }

    const outsideScan = scanNativeGrepFile(outsideCandidate, pattern, outsideSpy, &outsideSink);
    assert(outsideScan.error.empty && !outsideScan.stopped, outsideScan.error);
    assert(outsideRecords == ["a".replicate(8192) ~ "\x00needle"], outsideRecords.join(","));
    assert(outsideCounters.matcherCalls == 1, outsideCounters.matcherCalls.text);
    assert(outsideCounters.opens == 1 && outsideCounters.closes == 1,
            outsideCounters.opens.text ~ "/" ~ outsideCounters.closes.text);
    assert(outsideCounters.readCalls == 3, outsideCounters.readCalls.text);
    assert(outsideCounters.bytesRead == 8200, outsideCounters.bytesRead.text);
}

@("native_grep scanner matches multibyte UTF-8 straddling the prefix boundary")
unittest {
    import std.array : replicate;
    import std.file : rmdirRecurse;

    const base = freshNativeGrepTestDir("scan_utf8_straddle");
    scope (exit)
        rmdirRecurse(base);

    // The two bytes of the e-acute land at offsets 8191 and 8192.
    const content = "a".replicate(8191) ~ "éllo needle\n";
    writeNativeGrepFile(base ~ "/x.d", content);
    auto candidate = nativeGrepScanCandidate(base, "x.d");
    auto pattern = regex("needle");

    auto defaults = defaultNativeGrepOps();
    auto counters = NativeGrepSpyCounters();
    auto spy = nativeGrepCountingSpy(defaults, counters);

    string[] records;
    bool sink(ref const NativeGrepMatch match) {
        records ~= match.lineNumber.text ~ "|" ~ match.text;
        return false;
    }

    const scan = scanNativeGrepFile(candidate, pattern, spy, &sink);

    assert(scan.error.empty && !scan.stopped, scan.error);
    assert(records == ["1|" ~ "a".replicate(8191) ~ "éllo needle"], records.join(","));
    assert(counters.matcherCalls == 1, counters.matcherCalls.text);
    assert(counters.readCalls == 3, counters.readCalls.text);
    assert(counters.bytesRead == content.length, counters.bytesRead.text);
    assert(counters.opens == 1 && counters.closes == 1,
            counters.opens.text ~ "/" ~ counters.closes.text);
}

@("native_grep scanner fails malformed continuation bytes before any matcher call")
unittest {
    import std.array : replicate;
    import std.file : rmdirRecurse;

    const base = freshNativeGrepTestDir("scan_malformed_split");
    scope (exit)
        rmdirRecurse(base);

    // The matchable prefix sits inside the binary gate.
    // The malformed byte arrives in a later read on the same unterminated line.
    const content = "x".replicate(8190) ~ "needle\xFF\n";
    writeNativeGrepFile(base ~ "/x.d", content);
    auto candidate = nativeGrepScanCandidate(base, "x.d");
    auto pattern = regex("needle");

    auto defaults = defaultNativeGrepOps();
    auto counters = NativeGrepSpyCounters();
    auto spy = nativeGrepCountingSpy(defaults, counters);

    string[] records;
    bool sink(ref const NativeGrepMatch match) {
        records ~= match.text;
        return false;
    }

    const scan = scanNativeGrepFile(candidate, pattern, spy, &sink);

    assert(scan.error == "error: invalid UTF-8 in './x.d' at line 1", scan.error);
    assert(!scan.stopped);
    assert(records.empty, records.join(","));
    assert(counters.matcherCalls == 0, counters.matcherCalls.text);
    assert(counters.readCalls == 2, counters.readCalls.text);
    assert(counters.opens == 1 && counters.closes == 1,
            counters.opens.text ~ "/" ~ counters.closes.text);
}

@(
        "native_grep scanner frames empty, lf-only, mixed-ending, and cr-at-prefix-boundary files exactly")
unittest {
    import std.array : replicate;
    import std.file : rmdirRecurse;

    const base = freshNativeGrepTestDir("scan_line_endings");
    scope (exit)
        rmdirRecurse(base);

    writeNativeGrepFile(base ~ "/empty.d", "");
    writeNativeGrepFile(base ~ "/lf.d", "\n");
    writeNativeGrepFile(base ~ "/mixed.d", "a\r\nb\n\nc\r");
    writeNativeGrepFile(base ~ "/crbound.d", "a".replicate(8191) ~ "\r\nneedle\n");
    auto needle = regex("needle");
    auto matchAll = regex(".*");
    auto aPattern = regex("a");
    auto defaults = defaultNativeGrepOps();

    auto emptyCounters = NativeGrepSpyCounters();
    auto emptySpy = nativeGrepCountingSpy(defaults, emptyCounters);
    auto emptyCandidate = nativeGrepScanCandidate(base, "empty.d");
    string[] emptyRecords;
    bool emptySink(ref const NativeGrepMatch match) {
        emptyRecords ~= match.text;
        return false;
    }

    const emptyScan = scanNativeGrepFile(emptyCandidate, needle, emptySpy, &emptySink);
    assert(emptyScan.error.empty && !emptyScan.stopped, emptyScan.error);
    assert(emptyRecords.empty, emptyRecords.join(","));
    assert(emptyCounters.matcherCalls == 0, emptyCounters.matcherCalls.text);
    assert(emptyCounters.readCalls == 1, emptyCounters.readCalls.text);
    assert(emptyCounters.opens == 1 && emptyCounters.closes == 1,
            emptyCounters.opens.text ~ "/" ~ emptyCounters.closes.text);

    auto lfCounters = NativeGrepSpyCounters();
    auto lfSpy = nativeGrepCountingSpy(defaults, lfCounters);
    auto lfCandidate = nativeGrepScanCandidate(base, "lf.d");
    string[] lfRecords;
    bool lfSink(ref const NativeGrepMatch match) {
        lfRecords ~= match.text;
        return false;
    }

    const lfScan = scanNativeGrepFile(lfCandidate, needle, lfSpy, &lfSink);
    assert(lfScan.error.empty && !lfScan.stopped, lfScan.error);
    assert(lfRecords.empty, lfRecords.join(","));
    assert(lfCounters.matcherCalls == 1, lfCounters.matcherCalls.text);
    assert(lfCounters.readCalls == 2, lfCounters.readCalls.text);

    auto mixedCounters = NativeGrepSpyCounters();
    auto mixedSpy = nativeGrepCountingSpy(defaults, mixedCounters);
    auto mixedCandidate = nativeGrepScanCandidate(base, "mixed.d");
    string[] mixedRecords;
    bool mixedSink(ref const NativeGrepMatch match) {
        mixedRecords ~= match.lineNumber.text ~ "|" ~ match.text;
        return false;
    }

    const mixedScan = scanNativeGrepFile(mixedCandidate, matchAll, mixedSpy, &mixedSink);
    assert(mixedScan.error.empty && !mixedScan.stopped, mixedScan.error);
    assert(mixedRecords == ["1|a", "2|b", "3|", "4|c\r"], mixedRecords.join(","));
    assert(mixedCounters.matcherCalls == 4, mixedCounters.matcherCalls.text);
    assert(mixedCounters.bytesRead == 8, mixedCounters.bytesRead.text);
    assert(mixedCounters.opens == 1 && mixedCounters.closes == 1,
            mixedCounters.opens.text ~ "/" ~ mixedCounters.closes.text);

    // Keep a CR at offset 8191 pending when its LF arrives at offset 8192.
    // Pattern "a" exposes whether exactly one CR was stripped.
    auto crCounters = NativeGrepSpyCounters();
    auto crSpy = nativeGrepCountingSpy(defaults, crCounters);
    auto crCandidate = nativeGrepScanCandidate(base, "crbound.d");
    string[] crRecords;
    bool crSink(ref const NativeGrepMatch match) {
        crRecords ~= match.lineNumber.text ~ "|" ~ match.text;
        return false;
    }

    const crScan = scanNativeGrepFile(crCandidate, aPattern, crSpy, &crSink);
    assert(crScan.error.empty && !crScan.stopped, crScan.error);
    assert(crRecords == ["1|" ~ "a".replicate(8191)], crRecords.join(","));
    assert(crCounters.matcherCalls == 2, crCounters.matcherCalls.text);
    assert(crCounters.readCalls == 3, crCounters.readCalls.text);
    assert(crCounters.bytesRead == 8200, crCounters.bytesRead.text);
    assert(crCounters.opens == 1 && crCounters.closes == 1,
            crCounters.opens.text ~ "/" ~ crCounters.closes.text);
}

@("native_grep scanner retains match text byte-identical across later reads")
unittest {
    import std.array : replicate;
    import std.file : rmdirRecurse;

    const base = freshNativeGrepTestDir("scan_ownership");
    scope (exit)
        rmdirRecurse(base);

    const content = "needle-one\n" ~ "f".replicate(8200) ~ "needle-long\n"
        ~ "needle-two\n" ~ "f".replicate(4000) ~ "\n" ~ "needle-three\n";
    writeNativeGrepFile(base ~ "/x.d", content);
    auto candidate = nativeGrepScanCandidate(base, "x.d");
    auto pattern = regex("needle");

    auto defaults = defaultNativeGrepOps();
    auto counters = NativeGrepSpyCounters();
    auto spy = nativeGrepCountingSpy(defaults, counters);

    string[] records;
    bool sink(ref const NativeGrepMatch match) {
        records ~= match.lineNumber.text ~ "|" ~ match.text;
        return false;
    }

    const scan = scanNativeGrepFile(candidate, pattern, spy, &sink);

    assert(scan.error.empty && !scan.stopped, scan.error);
    assert(records == [
        "1|needle-one", "2|" ~ "f".replicate(8200) ~ "needle-long",
        "3|needle-two", "5|needle-three"
    ], records.join(","));
    assert(counters.matcherCalls == 5, counters.matcherCalls.text);
    assert(counters.readCalls == 3, counters.readCalls.text);
    assert(counters.bytesRead == content.length, counters.bytesRead.text);
    assert(counters.opens == 1 && counters.closes == 1,
            counters.opens.text ~ "/" ~ counters.closes.text);
}

@("native_grep scanner reports injected open failures without closing")
unittest {
    import std.algorithm.searching : canFind;
    import std.file : rmdirRecurse;

    const base = freshNativeGrepTestDir("scan_open_failure");
    scope (exit)
        rmdirRecurse(base);

    writeNativeGrepFile(base ~ "/x.d", "needle\n");
    auto candidate = nativeGrepScanCandidate(base, "x.d");
    auto pattern = regex("needle");

    auto defaults = defaultNativeGrepOps();
    auto counters = NativeGrepSpyCounters();
    auto spy = nativeGrepCountingSpy(defaults, counters);
    spy.open = (string p) {
        ++counters.opens;
        throw new Exception("open boom /abs/root/x.d secret detail");
    };

    string[] records;
    bool sink(ref const NativeGrepMatch match) {
        records ~= match.text;
        return false;
    }

    const scan = scanNativeGrepFile(candidate, pattern, spy, &sink);

    assert(scan.error == "error: file open failed for './x.d'", scan.error);
    assert(!scan.stopped);
    assert(records.empty, records.join(","));
    assert(counters.opens == 1 && counters.closes == 0,
            counters.opens.text ~ "/" ~ counters.closes.text);
    assert(counters.readCalls == 0 && counters.matcherCalls == 0,
            counters.readCalls.text ~ "/" ~ counters.matcherCalls.text);
    assert(!scan.error.canFind("/abs") && !scan.error.canFind("boom")
            && !scan.error.canFind("secret"), scan.error);
}

@("native_grep scanner reports injected read failures and still closes")
unittest {
    import std.algorithm.searching : canFind;
    import std.array : replicate;
    import std.file : rmdirRecurse;

    const base = freshNativeGrepTestDir("scan_read_failure");
    scope (exit)
        rmdirRecurse(base);

    writeNativeGrepFile(base ~ "/first.d", "needle\n");
    writeNativeGrepFile(base ~ "/second.d", "y".replicate(8192) ~ "needle\n");
    auto pattern = regex("needle");
    auto defaults = defaultNativeGrepOps();

    // The very first read throws after the open succeeded.
    auto firstCounters = NativeGrepSpyCounters();
    auto firstSpy = nativeGrepCountingSpy(defaults, firstCounters);
    firstSpy.read = (ref File f, ubyte[] b) {
        ++firstCounters.readCalls;
        throw new Exception("read boom /abs/root/first.d secret detail");
    };
    auto firstCandidate = nativeGrepScanCandidate(base, "first.d");
    string[] firstRecords;
    bool firstSink(ref const NativeGrepMatch match) {
        firstRecords ~= match.text;
        return false;
    }

    const firstScan = scanNativeGrepFile(firstCandidate, pattern, firstSpy, &firstSink);
    assert(firstScan.error == "error: file read failed for './first.d'", firstScan.error);
    assert(!firstScan.stopped);
    assert(firstRecords.empty, firstRecords.join(","));
    assert(firstCounters.closes == 1, firstCounters.closes.text);
    assert(firstCounters.matcherCalls == 0, firstCounters.matcherCalls.text);
    assert(!firstScan.error.canFind("/abs") && !firstScan.error.canFind("boom"), firstScan.error);

    // A clean prefix read followed by a throwing continuation read.
    auto secondCounters = NativeGrepSpyCounters();
    auto secondSpy = nativeGrepCountingSpy(defaults, secondCounters);
    auto secondCandidate = nativeGrepScanCandidate(base, "second.d");
    string[] secondRecords;
    bool secondSink(ref const NativeGrepMatch match) {
        secondRecords ~= match.text;
        return false;
    }

    size_t secondReads;
    secondSpy.read = (ref File f, ubyte[] b) {
        ++secondCounters.readCalls;
        if (++secondReads == 1) {
            const got = defaults.read(f, b);
            secondCounters.bytesRead += got;
            return got;
        }
        throw new Exception("read boom /abs/root/second.d secret detail");
    };

    const secondScan = scanNativeGrepFile(secondCandidate, pattern, secondSpy, &secondSink);
    assert(secondScan.error == "error: file read failed for './second.d'", secondScan.error);
    assert(secondRecords.empty, secondRecords.join(","));
    assert(secondCounters.readCalls == 2, secondCounters.readCalls.text);
    assert(secondCounters.closes == 1, secondCounters.closes.text);
    assert(secondCounters.matcherCalls == 0, secondCounters.matcherCalls.text);
}

@("native_grep scanner close exceptions preserve the selected outcome")
unittest {
    import std.file : rmdirRecurse;

    const base = freshNativeGrepTestDir("scan_close_failure");
    scope (exit)
        rmdirRecurse(base);

    writeNativeGrepFile(base ~ "/clean.d", "needle\n");
    writeNativeGrepFile(base ~ "/cap.d", "needle\nneedle\n");
    writeNativeGrepFile(base ~ "/binary.d", "\x00needle\n");
    auto pattern = regex("needle");
    auto defaults = defaultNativeGrepOps();

    NativeGrepOps makeThrowingCloseSpy(ref NativeGrepSpyCounters counters) {
        auto spy = nativeGrepCountingSpy(defaults, counters);
        spy.close = (ref File f) {
            ++counters.closes;
            throw new Exception("close boom /abs/root secret detail");
        };
        return spy;
    }

    string[] records;
    bool sink(ref const NativeGrepMatch match) {
        records ~= match.text;
        return false;
    }

    // Ordinary EOF keeps its clean no-error outcome.
    auto cleanCounters = NativeGrepSpyCounters();
    auto cleanSpy = makeThrowingCloseSpy(cleanCounters);
    auto cleanCandidate = nativeGrepScanCandidate(base, "clean.d");
    const cleanScan = scanNativeGrepFile(cleanCandidate, pattern, cleanSpy, &sink);
    assert(cleanScan.error.empty && !cleanScan.stopped, cleanScan.error);
    assert(records == ["needle"], records.join(","));
    assert(cleanCounters.closes == 1, cleanCounters.closes.text);

    // A cap stop keeps stopped=true and its record.
    records = [];
    auto capCounters = NativeGrepSpyCounters();
    auto capSpy = makeThrowingCloseSpy(capCounters);
    bool capSink(ref const NativeGrepMatch match) {
        records ~= match.text;
        return true;
    }

    auto capCandidate = nativeGrepScanCandidate(base, "cap.d");
    const capScan = scanNativeGrepFile(capCandidate, pattern, capSpy, &capSink);
    assert(capScan.stopped && capScan.error.empty, capScan.error);
    assert(records == ["needle"], records.join(","));
    assert(capCounters.closes == 1, capCounters.closes.text);

    // A binary skip is not turned into a failure by the close exception.
    records = [];
    auto binaryCounters = NativeGrepSpyCounters();
    auto binarySpy = makeThrowingCloseSpy(binaryCounters);
    auto binaryCandidate = nativeGrepScanCandidate(base, "binary.d");
    const binaryScan = scanNativeGrepFile(binaryCandidate, pattern, binarySpy, &sink);
    assert(binaryScan.error.empty && !binaryScan.stopped, binaryScan.error);
    assert(records.empty, records.join(","));
    assert(binaryCounters.matcherCalls == 0, binaryCounters.matcherCalls.text);
    assert(binaryCounters.closes == 1, binaryCounters.closes.text);
}
