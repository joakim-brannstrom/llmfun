module llm.tool_call.io.tests;

version (unittest) {
    import std.file : readText, mkdirRecurse;
    import std.stdio : File;
    import std.conv : to;
    import std.algorithm : canFind, startsWith;
    import std.array : empty;
    import std.json : parseJSON, JSONType;
    import std.typecons : Nullable;

    import my.path : AbsolutePath;

    import llm.config : VisionModelConfig, ToolLimits;
    import llm.tool_call.io.diff;
    import llm.tool_call.io.search;
    import llm.tool_call.io.diagnostic;
    import llm.tool_call.io.edit_engine;
    import llm.tool_call.io.edit_dispatch;
    import llm.tool_call.io.edit_tool;
    import llm.tool_call.io.functions;
    import llm.tool_call.io;

    // Mock FileContext for integration tests
    class TestFileContext : FileContext {
        AbsolutePath workAreaDir;

        this(string dir) {
            workAreaDir = dir.AbsolutePath;
            mkdirRecurse(workAreaDir.toString);
        }

        void teardown() {
            import std.file : rmdirRecurse;

            try {
                rmdirRecurse(workAreaDir.toString);
            } catch (Exception) {
                // Silently ignore cleanup failures during test teardown
            }
        }

        override bool isPathInsideWorkArea(AbsolutePath path) {
            auto p = path.toString;
            auto w = workAreaDir.toString;
            return p.startsWith(w) && (p.length == w.length || p[w.length] == '/');
        }

        override AbsolutePath workArea() {
            return workAreaDir;
        }

        override ToolLimits getToolLimits() {
            return ToolLimits();
        }
    }

    // Helper: create a test file with given content
    void createTestFile(TestFileContext ctx, string relativePath, string content) {
        assert(!relativePath.startsWith("/"), "relativePath must not be absolute: " ~ relativePath);
        auto fullPath = (ctx.workArea ~ relativePath).AbsolutePath;
        mkdirRecurse(fullPath.dirName.toString);
        auto f = File(fullPath.toString, "w");
        f.write(content);
    }

    // Helper: read file content for verification
    string readTestFile(TestFileContext ctx, string relativePath) {
        auto fullPath = (ctx.workArea ~ relativePath).AbsolutePath;
        return readText(fullPath.toString);
    }

    // Helper: create a test context with a unique directory name
    TestFileContext makeTestContext(string testName) {
        import std.datetime : Clock;

        return new TestFileContext("./llmfun_test/" ~ testName ~ "_" ~ Clock.currTime.toString);
    }
}

unittest {
    assert(parseEditMode("replace") == EditMode.replace);
    assert(parseEditMode("remove") == EditMode.remove);
    assert(parseEditMode("append") == EditMode.append);
    assert(parseEditMode("insert-after") == EditMode.insert_after);
    assert(parseEditMode("insert-before") == EditMode.insert_before);
    assert(parseEditMode("REPLACE") == EditMode.replace); // case-insensitive
    assert(parseEditMode("Remove") == EditMode.remove); // case-insensitive

    bool threw = false;
    try {
        parseEditMode("invalid");
    } catch (Exception) {
        threw = true;
    }
    assert(threw, "should throw on invalid mode");
}

unittest {
    string[] lines = ["hello world", "foo bar", "hello again"];

    // Exact substring match
    assert(findMarkerLine(lines, "hello world") == 0);

    // Substring match (marker is part of line)
    assert(findMarkerLine(lines, "world") == 0);
    assert(findMarkerLine(lines, "foo") == 1);

    // Multiple matches - should return first (index 0)
    assert(findMarkerLine(lines, "hello") == 0);

    // Case-sensitive: "Foo" does not match "foo"
    assert(findMarkerLine(lines, "Foo") == -1);
    assert(findMarkerLine(lines, "HELLO") == -1);

    // Not found
    assert(findMarkerLine(lines, "xyz") == -1);

    // Empty marker throws exception
    {
        bool threw = false;
        try {
            findMarkerLine(lines, "");
        } catch (Exception) {
            threw = true;
        }
        assert(threw, "empty marker should throw");
    }

    // Empty lines array
    assert(findMarkerLine(cast(string[])[], "xyz") == -1);

    // start offset: search begins at the given index
    assert(findMarkerLine(lines, "hello", 1) == 2); // second "hello" at index 2
    assert(findMarkerLine(lines, "world", 1) == -1); // only at index 0, skipped
    assert(findMarkerLine(lines, "foo", 2) == -1); // at index 1, skipped
    assert(findMarkerLine(lines, "hello", 3) == -1); // start beyond matches
    assert(findMarkerLine(lines, "hello", 100) == -1); // start beyond end
    assert(findMarkerLine(lines, "foo", 0) == 1); // start=0 behaves like default

    // endExclusive: search stops before the given index (scope support)
    assert(findMarkerLine(lines, "hello", 0, 1) == 0); // in [0, 1)
    assert(findMarkerLine(lines, "hello", 0, 2) == 0); // in [0, 2)
    assert(findMarkerLine(lines, "hello", 1, 2) == -1); // match at 2, excluded
    assert(findMarkerLine(lines, "hello", 1, 3) == 2); // match at 2, included
    assert(findMarkerLine(lines, "foo", 0, 1) == -1); // foo at 1, excluded
    assert(findMarkerLine(lines, "foo", 1, 2) == 1); // foo at 1, included
    assert(findMarkerLine(lines, "hello", 2, 2) == -1); // empty range
    assert(findMarkerLine(lines, "hello", 3, 2) == -1); // start > end
    assert(findMarkerLine(lines, "hello", 0, 100) == 0); // end beyond file clamps
}

unittest {
    string[] lines = ["a", "b", "a", "c", "a"];

    // findNthMarkerLine: Nth occurrence, 1-based
    assert(findNthMarkerLine(lines, "a", 1) == 0);
    assert(findNthMarkerLine(lines, "a", 2) == 2);
    assert(findNthMarkerLine(lines, "a", 3) == 4);
    assert(findNthMarkerLine(lines, "a", 4) == -1); // only 3 occurrences
    assert(findNthMarkerLine(lines, "zzz", 1) == -1); // not found at all

    // countMarkerOccurrences
    assert(countMarkerOccurrences(lines, "a") == 3);
    assert(countMarkerOccurrences(lines, "b") == 1);
    assert(countMarkerOccurrences(lines, "zzz") == 0);
    assert(countMarkerOccurrences(cast(string[])[], "a") == 0);

    // Range-limited variants (scope support)
    assert(findNthMarkerLine(lines, "a", 2, 1, 4) == -1); // only 1 "a" in [1, 4)
    assert(findNthMarkerLine(lines, "a", 1, 1, 4) == 2); // 1st "a" in [1, 4)
    assert(findNthMarkerLine(lines, "a", 3, 1, 4) == -1); // only 1 in [1, 4)
    assert(findNthMarkerLine(lines, "a", 2, 0, 3) == 2); // 2nd "a" at index 2
    assert(countMarkerOccurrences(lines, "a", 1, 4) == 1);
    assert(countMarkerOccurrences(lines, "a", 0, 3) == 2);
    assert(countMarkerOccurrences(lines, "a", 5, 100) == 0); // start beyond end
    assert(countMarkerOccurrences(lines, "a", 0, 100) == 3); // clamps to file
}

unittest {
    // === Exact match ===
    {
        string[] fileLines = ["foo", "bar", "baz"];
        string[] searchLines = ["foo", "bar"];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(result.found);
        assert(result.start == 0);
        assert(result.end == 2);
    }

    // === Whitespace tolerance via strip ===
    {
        string[] fileLines = ["    foo", "  bar  ", "baz"];
        string[] searchLines = ["foo", "bar"];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(result.found);
        assert(result.start == 0);
        assert(result.end == 2);
    }

    // === Tabs vs spaces tolerance ===
    {
        string[] fileLines = ["\t\tfoo", "\tbar"];
        string[] searchLines = ["foo", "bar"];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(result.found);
        assert(result.start == 0);
        assert(result.end == 2);
    }

    // === Comment false positive prevention ===
    // Searching for "foo" should NOT match "// call fooBar()"
    {
        string[] fileLines = ["// call fooBar()", "other"];
        string[] searchLines = ["foo"];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(!result.found);
    }

    // === Not found ===
    {
        string[] fileLines = ["hello", "world"];
        string[] searchLines = ["foo"];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(!result.found);
    }

    // === First match when multiple matches exist ===
    {
        string[] fileLines = ["foo", "bar", "foo", "bar", "extra"];
        string[] searchLines = ["foo", "bar"];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(result.found);
        assert(result.start == 0);
        assert(result.end == 2);
    }

    // === Empty search block returns not found ===
    {
        auto result = findCodeBlock(["foo"], cast(string[])[]);
        assert(!result.found);
    }

    // === All-empty search block returns not found ===
    {
        auto result = findCodeBlock(["foo"], ["", "  ", "\t"]);
        assert(!result.found);
    }

    // === Leading empty lines in search content ===
    {
        string[] fileLines = ["foo", "bar"];
        string[] searchLines = ["", "", "foo", "bar"];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(result.found);
        assert(result.start == 0);
        assert(result.end == 2);
    }

    // === Multi-line search block matching ===
    {
        string[] fileLines = ["function foo() {", "    return bar;", "}"];
        string[] searchLines = ["function foo() {", "    return bar;", "}"];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(result.found);
        assert(result.start == 0);
        assert(result.end == 3);
    }

    // === Empty search lines in middle (flexible) ===
    {
        string[] fileLines = ["function foo() {", "    return bar;", "}"];
        string[] searchLines = [
            "function foo() {", "", "    return bar;", "", "}"
        ];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(result.found);
        assert(result.start == 0);
        assert(result.end == 3);
    }

    // === Match not at start of file ===
    {
        string[] fileLines = ["header", "foo", "bar", "footer"];
        string[] searchLines = ["foo", "bar"];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(result.found);
        assert(result.start == 1);
        assert(result.end == 3);
    }

    // === Single-line search block ===
    {
        string[] fileLines = ["hello", "world", "foo"];
        string[] searchLines = ["world"];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(result.found);
        assert(result.start == 1);
        assert(result.end == 2);
    }

    // === Empty file lines ===
    {
        auto result = findCodeBlock(cast(string[])[], ["foo"]);
        assert(!result.found);
    }
    // === Search block longer than file ===
    {
        string[] fileLines = ["foo"];
        string[] searchLines = ["foo", "bar", "baz"];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(!result.found);
    }

    // === Anchor at end of file with trailing search lines ===
    {
        string[] fileLines = ["header", "anchor"];
        string[] searchLines = ["anchor", "trailing"];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(!result.found);
    }

    // === CRLF line endings tolerance ===
    {
        string[] fileLines = ["foo\r", "bar\r"];
        string[] searchLines = ["foo", "bar"];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(result.found);
        assert(result.start == 0);
        assert(result.end == 2);
    }

    // === start offset: search begins at the given file line ===
    {
        string[] fileLines = ["foo", "bar", "foo", "bar", "end"];
        string[] searchLines = ["foo", "bar"];
        auto result = findCodeBlock(fileLines, searchLines, 2);
        assert(result.found);
        assert(result.start == 2);
        assert(result.end == 4);
    }

    // === start offset skips earlier matches ===
    {
        string[] fileLines = ["foo", "bar", "foo", "bar"];
        string[] searchLines = ["foo", "bar"];
        auto result = findCodeBlock(fileLines, searchLines, 1);
        assert(result.found);
        assert(result.start == 2);
        assert(result.end == 4);
    }

    // === start offset beyond all matches ===
    {
        string[] fileLines = ["foo", "bar", "baz"];
        string[] searchLines = ["foo"];
        auto result = findCodeBlock(fileLines, searchLines, 3);
        assert(!result.found);
    }

    // === start offset at end of file ===
    {
        string[] fileLines = ["foo"];
        string[] searchLines = ["foo"];
        auto result = findCodeBlock(fileLines, searchLines, 1);
        assert(!result.found);
    }
}

unittest {
    string[] fileLines = ["foo", "bar", "foo", "bar", "foo", "bar", "end"];
    string[] searchLines = ["foo", "bar"];
    // findNthCodeBlock: Nth non-overlapping occurrence, 1-based
    assert(findNthCodeBlock(fileLines, searchLines, 1).found);
    assert(findNthCodeBlock(fileLines, searchLines, 1).start == 0);
    assert(findNthCodeBlock(fileLines, searchLines, 2).found);
    assert(findNthCodeBlock(fileLines, searchLines, 2).start == 2);
    assert(findNthCodeBlock(fileLines, searchLines, 3).found);
    assert(findNthCodeBlock(fileLines, searchLines, 3).start == 4);
    assert(!findNthCodeBlock(fileLines, searchLines, 4).found); // only 3

    // countCodeBlockOccurrences
    assert(countCodeBlockOccurrences(fileLines, searchLines) == 3);
    assert(countCodeBlockOccurrences(["foo", "bar", "end"], searchLines) == 1);
    assert(countCodeBlockOccurrences(["foo", "end"], searchLines) == 0);
    assert(countCodeBlockOccurrences(cast(string[])[], searchLines) == 0);

    // Range-limited variants (scope support): only the anchor is constrained
    assert(findCodeBlock(fileLines, searchLines, 0, 4).found);
    assert(findCodeBlock(fileLines, searchLines, 0, 4).start == 0);
    assert(findCodeBlock(fileLines, searchLines, 2, 4).found);
    assert(findCodeBlock(fileLines, searchLines, 2, 4).start == 2);
    assert(!findCodeBlock(fileLines, searchLines, 3, 4).found); // anchor at 4 excluded
    assert(findCodeBlock(fileLines, searchLines, 4, 7).found); // anchor at 4 ok
    assert(!findCodeBlock(fileLines, searchLines, 4, 4).found); // empty range
    assert(!findCodeBlock(fileLines, searchLines, 6, 2).found); // start > end
    // A block whose anchor is in range may extend past the end
    assert(findCodeBlock(fileLines, searchLines, 0, 1).found);
    assert(findCodeBlock(fileLines, searchLines, 0, 1).end == 2);

    assert(findNthCodeBlock(fileLines, searchLines, 1, 2, 6).found);
    assert(findNthCodeBlock(fileLines, searchLines, 1, 2, 6).start == 2);
    assert(findNthCodeBlock(fileLines, searchLines, 2, 2, 6).found);
    assert(findNthCodeBlock(fileLines, searchLines, 2, 2, 6).start == 4);
    assert(!findNthCodeBlock(fileLines, searchLines, 3, 2, 6).found); // only 2 in range
    assert(countCodeBlockOccurrences(fileLines, searchLines, 2, 6) == 2);
    assert(countCodeBlockOccurrences(fileLines, searchLines, 0, 3) == 2); // anchors at 0 and 2
    assert(countCodeBlockOccurrences(fileLines, searchLines, 6, 100) == 0);
}

unittest {
    // === replace: 3-line block replaced with 2-line content -> linesChanged = -1 ===
    {
        string[] fileLines = ["a", "b", "c", "d", "e", "f"];
        auto res = editFileInMemory(fileLines, EditMode.replace, "x\ny", EditTarget(2, 5, 3, 3));
        assert(res.lines == ["a", "b", "x", "y", "f"], res.lines.to!string);
        assert(res.linesChanged == -1, res.linesChanged.to!string);
    }

    // === remove: 5 lines deleted -> linesChanged = -5 ===
    {
        string[] fileLines = ["a", "b", "c", "d", "e", "f", "g"];
        auto res = editFileInMemory(fileLines, EditMode.remove, "", EditTarget(1, 6, 2, 5));
        assert(res.lines == ["a", "g"], res.lines.to!string);
        assert(res.linesChanged == -5, res.linesChanged.to!string);
    }

    // === append: content inserted after line -> linesChanged = contentLineCount ===
    {
        string[] fileLines = ["a", "b", "c"];
        auto res = editFileInMemory(fileLines, EditMode.append, "x\ny", EditTarget(1, 1, 2, 1));
        assert(res.lines == ["a", "b", "x", "y", "c"], res.lines.to!string);
        assert(res.linesChanged == 2, res.linesChanged.to!string);
    }

    // === insert_after: alias for append ===
    {
        string[] fileLines = ["a", "b", "c"];
        auto res = editFileInMemory(fileLines, EditMode.insert_after, "x", EditTarget(1, 1, 2, 1));
        assert(res.lines == ["a", "b", "x", "c"], res.lines.to!string);
        assert(res.linesChanged == 1, res.linesChanged.to!string);
    }

    // === insert_before: content inserted before line -> linesChanged = contentLineCount ===
    {
        string[] fileLines = ["a", "b", "c"];
        auto res = editFileInMemory(fileLines, EditMode.insert_before, "x\ny",
                EditTarget(1, 1, 2, 1));
        assert(res.lines == ["a", "x", "y", "b", "c"], res.lines.to!string);
        assert(res.linesChanged == 2, res.linesChanged.to!string);
    }

    // === content preserved exactly: leading/trailing whitespace and tabs kept ===
    {
        string[] fileLines = ["one", "two"];
        auto res = editFileInMemory(fileLines, EditMode.replace,
                "  indented\tline\n\t\ttabbed\n", EditTarget(0, 1, 1, 1));
        assert(res.lines == ["  indented\tline", "\t\ttabbed", "two"], res.lines.to!string);
        assert(res.linesChanged == 1, res.linesChanged.to!string);
    }

    // === newline-only content means one blank line, not zero lines ===
    {
        string[] fileLines = ["a", "b"];
        auto res = editFileInMemory(fileLines, EditMode.replace, "\n", EditTarget(0, 1, 1, 1));
        assert(res.lines == ["", "b"], res.lines.to!string);
        assert(res.linesChanged == 0, res.linesChanged.to!string);
    }

    // === two newlines also mean one blank line (last newline terminates it) ===
    {
        string[] fileLines = ["a", "b"];
        auto res = editFileInMemory(fileLines, EditMode.replace, "\n\n", EditTarget(0, 1, 1, 1));
        assert(res.lines == ["", "b"], res.lines.to!string);
        assert(res.linesChanged == 0, res.linesChanged.to!string);
    }

    // === newline-only content in append mode inserts a blank line ===
    {
        string[] fileLines = ["a", "b"];
        auto res = editFileInMemory(fileLines, EditMode.append, "\n", EditTarget(1, 1, 2, 1));
        assert(res.lines == ["a", "b", ""], res.lines.to!string);
        assert(res.linesChanged == 1, res.linesChanged.to!string);
    }

    // === CRLF line endings in content are split correctly ===
    {
        string[] fileLines = ["a", "b"];
        auto res = editFileInMemory(fileLines, EditMode.replace, "x\r\ny", EditTarget(0, 1, 1, 1));
        assert(res.lines == ["x", "y", "b"], res.lines.to!string);
        assert(res.linesChanged == 1, res.linesChanged.to!string);
    }

    // === empty file: replace [0,0) with content ===
    {
        auto res = editFileInMemory(cast(string[])[], EditMode.replace, "a\nb",
                EditTarget(0, 0, 0, 0));
        assert(res.lines == ["a", "b"], res.lines.to!string);
        assert(res.linesChanged == 2, res.linesChanged.to!string);
    }

    // === empty file: append with startLine=0 creates content ===
    {
        auto res = editFileInMemory(cast(string[])[], EditMode.append, "a",
                EditTarget(0, 0, 0, 0));
        assert(res.lines == ["a"], res.lines.to!string);
        assert(res.linesChanged == 1, res.linesChanged.to!string);
    }

    // === empty file: insert_before with startLine=0 creates content ===
    {
        auto res = editFileInMemory(cast(string[])[], EditMode.insert_before,
                "a", EditTarget(0, 0, 0, 0));
        assert(res.lines == ["a"], res.lines.to!string);
        assert(res.linesChanged == 1, res.linesChanged.to!string);
    }

    // === empty file: remove [0,0) is a no-op ===
    {
        auto res = editFileInMemory(cast(string[])[], EditMode.remove, "", EditTarget(0, 0, 0, 0));
        assert(res.lines.length == 0, res.lines.to!string);
        assert(res.linesChanged == 0, res.linesChanged.to!string);
    }

    // === single-line file: replace ===
    {
        string[] fileLines = ["only"];
        auto res = editFileInMemory(fileLines, EditMode.replace, "a\nb", EditTarget(0, 1, 1, 1));
        assert(res.lines == ["a", "b"], res.lines.to!string);
        assert(res.linesChanged == 1, res.linesChanged.to!string);
    }

    // === single-line file: remove ===
    {
        string[] fileLines = ["only"];
        auto res = editFileInMemory(fileLines, EditMode.remove, "", EditTarget(0, 1, 1, 1));
        assert(res.lines.length == 0, res.lines.to!string);
        assert(res.linesChanged == -1, res.linesChanged.to!string);
    }

    // === single-line file: append ===
    {
        string[] fileLines = ["only"];
        auto res = editFileInMemory(fileLines, EditMode.append, "a", EditTarget(0, 0, 1, 1));
        assert(res.lines == ["only", "a"], res.lines.to!string);
        assert(res.linesChanged == 1, res.linesChanged.to!string);
    }

    // === append at end of file (anchor = last line) ===
    {
        string[] fileLines = ["a", "b", "c"];
        auto res = editFileInMemory(fileLines, EditMode.append, "d", EditTarget(2, 2, 3, 1));
        assert(res.lines == ["a", "b", "c", "d"], res.lines.to!string);
        assert(res.linesChanged == 1, res.linesChanged.to!string);
    }

    // === append with startLine == file length also appends at end ===
    {
        string[] fileLines = ["a", "b", "c"];
        auto res = editFileInMemory(fileLines, EditMode.append, "d", EditTarget(3, 3, 4, 1));
        assert(res.lines == ["a", "b", "c", "d"], res.lines.to!string);
        assert(res.linesChanged == 1, res.linesChanged.to!string);
    }

    // === insert_before at start of file ===
    {
        string[] fileLines = ["a", "b"];
        auto res = editFileInMemory(fileLines, EditMode.insert_before, "x",
                EditTarget(0, 0, 1, 1));
        assert(res.lines == ["x", "a", "b"], res.lines.to!string);
        assert(res.linesChanged == 1, res.linesChanged.to!string);
    }

    // === remove mode with non-empty content throws ===
    {
        bool threw = false;
        try {
            editFileInMemory(["a", "b"], EditMode.remove, "not empty", EditTarget(0, 1, 1, 1));
        } catch (Exception e) {
            threw = true;
            assert(e.msg.canFind("empty"), e.msg);
        }
        assert(threw, "remove mode with non-empty content must throw");
    }

    // === replace with empty content deletes the range ===
    {
        string[] fileLines = ["a", "b", "c", "d"];
        auto res = editFileInMemory(fileLines, EditMode.replace, "", EditTarget(1, 3, 2, 2));
        assert(res.lines == ["a", "d"], res.lines.to!string);
        assert(res.linesChanged == -2, res.linesChanged.to!string);
    }

    // === matched target is echoed in the result ===
    {
        string[] fileLines = ["a", "b", "c"];
        auto res = editFileInMemory(fileLines, EditMode.replace, "x", EditTarget(1, 2, 2, 1));
        assert(res.matched.startLine == 1, res.matched.startLine.to!string);
        assert(res.matched.endLine == 2, res.matched.endLine.to!string);
        assert(res.matched.matchedAt == 2, res.matched.matchedAt.to!string);
        assert(res.matched.matchedLines == 1, res.matched.matchedLines.to!string);
    }

    // === invalid range: endLine < startLine throws ===
    {
        bool threw = false;
        try {
            editFileInMemory(["a", "b"], EditMode.replace, "x", EditTarget(2, 1, 1, 1));
        } catch (Exception) {
            threw = true;
        }
        assert(threw, "endLine < startLine must throw");
    }

    // === invalid range: endLine beyond file length throws ===
    {
        bool threw = false;
        try {
            editFileInMemory(["a", "b"], EditMode.remove, "", EditTarget(0, 5, 1, 1));
        } catch (Exception) {
            threw = true;
        }
        assert(threw, "endLine beyond file length must throw");
    }

    // === invalid range: negative startLine throws ===
    {
        bool threw = false;
        try {
            editFileInMemory(["a", "b"], EditMode.append, "x", EditTarget(-1, 0, 1, 1));
        } catch (Exception) {
            threw = true;
        }
        assert(threw, "negative startLine must throw");
    }

    // === original fileLines array is not mutated ===
    {
        string[] fileLines = ["a", "b", "c", "d"];
        string[] original = fileLines.dup;
        editFileInMemory(fileLines, EditMode.replace, "x\ny", EditTarget(1, 3, 2, 2));
        assert(fileLines == original, "original fileLines must not be mutated");
    }
}

unittest {
    // Structured logging helpers (Task 6).
    assert(sanitizeLogPath("a/b/c.txt") == "a/b/c.txt");
    assert(sanitizeLogPath("./a/b.txt") == "a/b.txt");
    assert(sanitizeLogPath("../secret/../x.txt") == "secret/x.txt");
    assert(sanitizeLogPath("/etc/passwd") == "etc/passwd");
    assert(sanitizeLogPath("") == "(workarea)");
    assert(sanitizeLogPath("../..") == "(workarea)");

    assert(classifyEditError("marker 'zzz' not found in file") == "search_not_found");
    assert(classifyEditError("search block not found in file") == "search_not_found");
    assert(classifyEditError(
            "ambiguous targeting: multiple targeting parameters provided (startLine, marker)") == "ambiguous_targeting");
    assert(classifyEditError(
            "no targeting method specified. Provide exactly one of: startLine, marker, searchContent") == "missing_targeting");
    assert(classifyEditError(
            "Hunk 1: Context mismatch at line 3: expected 'x' but found 'y'") == "context_mismatch");
    assert(classifyEditError("marker at line 3 with count 5 exceeds file length") == "range_oob");
    assert(classifyEditError("Diff does not contain any hunk header (@@ ... @@)") == "invalid_diff");
    assert(classifyEditError(
            "Hunk 1: Invalid hunk line (must start with ' ', '-' or '+')") == "invalid_diff");
    assert(classifyEditError("parameter matchIndex 0 must be >= 1") == "invalid_parameter");
    assert(classifyEditError(
            "replaceAll is not supported with byLine targeting") == "invalid_parameter");
    assert(classifyEditError(
            "matchIndex=3 but only 2 occurrences of marker 'a' were found") == "match_index_oob");
    assert(classifyEditError(
            "matchIndex=2 but only 1 occurrence of the search block was found") == "match_index_oob");
    assert(classifyEditError(
            "matchIndex > 1 cannot be combined with replaceAll") == "invalid_parameter");
    assert(classifyEditError("some unknown failure") == "other");

    assert(classifyEditError(
            "marker 'zzz' not found in file within scope [10, 20]") == "search_not_found");
    assert(classifyEditError(
            "search block not found in file within scope from line 10") == "search_not_found");
    assert(classifyEditError("parameter scopeStart 0 must be >= 1") == "invalid_parameter");
    assert(classifyEditError(
            "parameter scopeStart (20) must be <= scopeEnd (10)") == "invalid_parameter");

    assert(hunkNumberFrom("Hunk 2: Context mismatch at line 5: expected 'x' but found 'y'") == 2);
    assert(hunkNumberFrom("Hunk 12: Unexpected end of file at line 4 (hunk context)") == 12);
    assert(hunkNumberFrom("Hunk 2: Hunk tries to go backward (oldStart=3)") == 2);
    assert(hunkNumberFrom("Diff does not contain any hunk header") == -1);
    assert(hunkNumberFrom("") == -1);

    assert(targetingMethodOf(UnifiedEditFileParams("p.txt", "x", "replace",
            startLine: 1, count: 1)) == "byLine");
    assert(targetingMethodOf(UnifiedEditFileParams("p.txt", "x", "replace",
            marker: "m")) == "byMarker");
    assert(targetingMethodOf(UnifiedEditFileParams("p.txt", "x", "replace",
            searchContent: "s")) == "byContent");
    assert(targetingMethodOf(UnifiedEditFileParams("p.txt", "x", "replace")) == "none");
    assert(targetingMethodOf(UnifiedEditFileParams("p.txt", "x", "replace",
            startLine: 1, marker: "m")) == "none");

    assert(targetingMethodOf(UnifiedEditFileParams("p.txt", "x", "replace",
            marker: "m", scopeStart: 5, scopeEnd: 10)) == "byMarker");

    // Scope helpers (Task 10).
    assert(scopeDescription(10, 20) == "within scope [10, 20]");
    assert(scopeDescription(10, -1) == "within scope from line 10");
    assert(scopeDescription(-1, 20) == "within scope up to line 20");
    assert(scopeDescription(-1, -1) == "");
    assert(appendScope("marker 'x' not found in file", -1, -1) == "marker 'x' not found in file");
    assert(appendScope("marker 'x' not found in file", 10,
            20) == "marker 'x' not found in file within scope [10, 20]");
    assert(scopeLogValue(10, 20) == "10-20");
    assert(scopeLogValue(10, -1) == "from-10");
    assert(scopeLogValue(-1, 20) == "up-to-20");
    assert(scopeLogValue(-1, -1) == "none");
}

unittest {
    // === byLine + replace: replaces correct lines, returns correct metadata ===
    string[] fileLines = ["a", "b", "c", "d", "e"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "X\nY", startLine: 2, count: 2);
    assert(res.lines == ["a", "X", "Y", "d", "e"], res.lines.to!string);
    assert(res.matched.matchedAt == 2);
    assert(res.matched.matchedLines == 2);
    assert(res.linesChanged == 0, res.linesChanged.to!string);
    assert(res.operations == 1);
    assert(!res.autoCountUsed);
}

unittest {
    // === byMarker + replace + multi-line content + no count → auto-count ===
    string[] fileLines = ["a", "marker", "b", "c", "d"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "X\nY\nZ", marker: "marker");
    assert(res.lines == ["a", "X", "Y", "Z", "d"], res.lines.to!string);
    assert(res.matched.matchedAt == 2);
    assert(res.matched.matchedLines == 3, res.matched.matchedLines.to!string);
    assert(res.autoCountUsed);
    assert(res.linesChanged == 0, res.linesChanged.to!string);
}

unittest {
    // === byMarker + replace + single-line content + no count → count=1 ===
    string[] fileLines = ["a", "marker", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "X", marker: "marker");
    assert(res.lines == ["a", "X", "b"], res.lines.to!string);
    assert(res.matched.matchedLines == 1);
    assert(!res.autoCountUsed);
}

unittest {
    // === byMarker + replace + multi-line content + explicit count=1 ===
    string[] fileLines = ["a", "marker", "b", "c"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "X\nY",
            marker: "marker", count: 1);
    assert(res.lines == ["a", "X", "Y", "b", "c"], res.lines.to!string);
    assert(res.matched.matchedLines == 1);
    assert(!res.autoCountUsed);
}

unittest {
    // === byMarker + replace + explicit count > content lines ===
    string[] fileLines = ["a", "marker", "b", "c", "d"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "X", marker: "marker", count: 3);
    assert(res.lines == ["a", "X", "d"], res.lines.to!string);
    assert(res.linesChanged == -2, res.linesChanged.to!string);
    assert(res.matched.matchedLines == 3);
}

unittest {
    // === byMarker + auto-count exceeds file length → error ===
    bool threw = false;
    try {
        editFileUnifiedMemory(["a", "marker"], EditMode.replace, "X\nY\nZ", marker: "marker");
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("exceeds file length"), e.msg);
    }
    assert(threw, "auto-count beyond EOF must error");
}

unittest {
    // === byContent + replace: block found by trimmed equality ===
    string[] fileLines = ["a", "foo() {", "  bar();", "}", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "new()",
            searchContent: "foo() {\n  bar();\n}");
    assert(res.lines == ["a", "new()", "b"], res.lines.to!string);
    assert(res.matched.matchedAt == 2);
    assert(res.matched.matchedLines == 3, res.matched.matchedLines.to!string);
}

unittest {
    // === byContent + replaceAll: all occurrences replaced, non-overlapping ===
    string[] fileLines = ["x", "foo", "y", "foo", "z"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "bar",
            searchContent: "foo", replaceAll: true);
    assert(res.lines == ["x", "bar", "y", "bar", "z"], res.lines.to!string);
    assert(res.operations == 2, res.operations.to!string);
    assert(res.matched.matchedAt == 2);
}

unittest {
    // === byContent + replaceAll is non-overlapping: replacement content that
    // contains the search pattern is not re-matched ===
    string[] fileLines = ["a", "b", "c"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "b\nx",
            searchContent: "b", replaceAll: true);
    assert(res.lines == ["a", "b", "x", "c"], res.lines.to!string);
    assert(res.operations == 1, res.operations.to!string);
}

unittest {
    // === byMarker + replaceAll: each marker line replaced ===
    string[] fileLines = ["a", "marker", "b", "marker", "c"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "X", marker: "marker",
            replaceAll: true);
    assert(res.lines == ["a", "X", "b", "X", "c"], res.lines.to!string);
    assert(res.operations == 2);
    assert(res.matched.matchedAt == 2);
}

unittest {
    // === append with byMarker keeps the marker line ===
    string[] fileLines = ["a", "marker", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.append, "X", marker: "marker");
    assert(res.lines == ["a", "marker", "X", "b"], res.lines.to!string);
    assert(res.matched.matchedAt == 2);
}

unittest {
    // === insert_before with byContent inserts before the matched block ===
    string[] fileLines = ["a", "foo", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.insert_before, "X", searchContent: "foo");
    assert(res.lines == ["a", "X", "foo", "b"], res.lines.to!string);
}

unittest {
    // === remove with byContent deletes the matched block ===
    string[] fileLines = ["a", "foo", "bar", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.remove, "", searchContent: "foo\nbar");
    assert(res.lines == ["a", "b"], res.lines.to!string);
    assert(res.linesChanged == -2);
}

unittest {
    // === byContent + explicit count extends the replaced range ===
    string[] fileLines = ["a", "foo", "bar", "baz", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "X",
            searchContent: "foo", count: 3);
    assert(res.lines == ["a", "X", "b"], res.lines.to!string);
    assert(res.matched.matchedLines == 3);
}

unittest {
    // === empty content in replace mode deletes the targeted lines ===
    string[] fileLines = ["a", "marker", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "", marker: "marker");
    assert(res.lines == ["a", "b"], res.lines.to!string);
    assert(res.linesChanged == -1);
}

unittest {
    // === error: ambiguous targeting (multiple methods) ===
    bool threw = false;
    try {
        editFileUnifiedMemory(["a"], EditMode.replace, "x", startLine: 1, marker: "a");
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("ambiguous"), e.msg);
        assert(e.msg.canFind("startLine") && e.msg.canFind("marker"), e.msg);
    }
    assert(threw);
}

unittest {
    // === error: missing targeting (no method) ===
    bool threw = false;
    try {
        editFileUnifiedMemory(["a"], EditMode.replace, "x");
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("no targeting method"), e.msg);
    }
    assert(threw);
}

unittest {
    // === error: byMarker not found ===
    bool threw = false;
    try {
        editFileUnifiedMemory(["a", "b"], EditMode.replace, "x", marker: "zzz");
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("not found"), e.msg);
    }
    assert(threw);
}

unittest {
    // === error: byContent not found ===
    bool threw = false;
    try {
        editFileUnifiedMemory(["a", "b"], EditMode.replace, "x", searchContent: "zzz");
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("not found"), e.msg);
    }
    assert(threw);
}

unittest {
    // === error: replaceAll with byLine is not supported ===
    bool threw = false;
    try {
        editFileUnifiedMemory(["a", "b"], EditMode.replace, "x", startLine: 1,
                count: 1, replaceAll: true);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("replaceAll"), e.msg);
    }
    assert(threw);
}

unittest {
    // === error: replaceAll with insert modes is not supported ===
    bool threw = false;
    try {
        editFileUnifiedMemory(["a"], EditMode.append, "x", marker: "a", replaceAll: true);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("replaceAll"), e.msg);
    }
    assert(threw);
}

unittest {
    // === matchIndex=2 targets the SECOND occurrence (byMarker) ===
    string[] fileLines = ["a", "b", "a", "c"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "x", marker: "a", matchIndex: 2);
    assert(res.lines == ["a", "b", "x", "c"], res.lines.to!string);
    assert(res.matched.matchedAt == 3, res.matched.matchedAt.to!string);
    assert(res.matched.matchedLines == 1);
}

unittest {
    // === matchIndex=3 targets the THIRD occurrence (byContent) ===
    string[] fileLines = ["foo", "bar", "foo", "bar", "foo", "bar", "end"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "X",
            searchContent: "foo\nbar", matchIndex: 3);
    assert(res.lines == ["foo", "bar", "foo", "bar", "X", "end"], res.lines.to!string);
    assert(res.matched.matchedAt == 5, res.matched.matchedAt.to!string);
    assert(res.matched.matchedLines == 2);
}

unittest {
    // === matchIndex OOB reports actual occurrence count (byMarker) ===
    bool threw = false;
    try {
        editFileUnifiedMemory(["a", "b", "a"], EditMode.replace, "x", marker: "a", matchIndex: 3);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("matchIndex=3") && e.msg.canFind("2 occurrences"), e.msg);
    }
    assert(threw);
}

unittest {
    // === matchIndex OOB reports actual occurrence count (byContent) ===
    bool threw = false;
    try {
        editFileUnifiedMemory(["foo", "bar", "foo"], EditMode.replace, "x",
                searchContent: "foo\nbar", matchIndex: 2);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("matchIndex=2") && e.msg.canFind("1 occurrence"), e.msg);
    }
    assert(threw);
}

unittest {
    // === matchIndex > 1 with replaceAll is rejected ===
    bool threw = false;
    try {
        editFileUnifiedMemory(["a", "a"], EditMode.replace, "x", marker: "a",
                replaceAll: true, matchIndex: 2);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("matchIndex") && e.msg.canFind("replaceAll"), e.msg);
    }
    assert(threw);
}

unittest {
    // === byLine + replaceAll + matchIndex>1: byLine error takes precedence
    // (matchIndex is documented as ignored by byLine) ===
    bool threw = false;
    try {
        editFileUnifiedMemory(["a", "b"], EditMode.replace, "x", startLine: 1,
                count: 1, replaceAll: true, matchIndex: 2);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("replaceAll is not supported with byLine"), e.msg);
    }
    assert(threw);
}

unittest {
    // === matchIndex with byLine is ignored ===
    string[] fileLines = ["a", "b", "c"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "x",
            startLine: 2, count: 1, matchIndex: 2);
    assert(res.lines == ["a", "x", "c"], res.lines.to!string);
    assert(res.matched.matchedAt == 2);
}

unittest {
    // === scope: byMarker found within scope ===
    string[] fileLines = ["a", "target", "c", "d"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "x", marker: "target",
            scopeStart: 2, scopeEnd: 3);
    assert(res.lines == ["a", "x", "c", "d"], res.lines.to!string);
    assert(res.matched.matchedAt == 2, res.matched.matchedAt.to!string);
}

unittest {
    // === scope: byMarker outside scope → not found, message mentions scope ===
    string[] fileLines = ["target", "b", "c", "d"];
    bool threw = false;
    try {
        editFileUnifiedMemory(fileLines, EditMode.replace, "x", marker: "target",
                scopeStart: 2, scopeEnd: 4);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("not found"), e.msg);
        assert(e.msg.canFind("within scope [2, 4]"), e.msg);
    }
    assert(threw);
}

unittest {
    // === scope: byMarker after scopeEnd → not found ===
    string[] fileLines = ["a", "b", "c", "target"];
    bool threw = false;
    try {
        editFileUnifiedMemory(fileLines, EditMode.replace, "x", marker: "target",
                scopeStart: 1, scopeEnd: 3);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("within scope [1, 3]"), e.msg);
    }
    assert(threw);
}

unittest {
    // === scope: scopeStart alone searches to EOF ===
    string[] fileLines = ["target", "b", "c", "target2"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "x", marker: "target2",
            scopeStart: 4);
    assert(res.lines == ["target", "b", "c", "x"], res.lines.to!string);
    assert(res.matched.matchedAt == 4, res.matched.matchedAt.to!string);
}

unittest {
    // === scope: scopeEnd alone searches from line 1 ===
    string[] fileLines = ["a", "target", "c", "target2"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "x", marker: "target",
            scopeEnd: 2);
    assert(res.lines == ["a", "x", "c", "target2"], res.lines.to!string);
    assert(res.matched.matchedAt == 2, res.matched.matchedAt.to!string);
}

unittest {
    // === scope: byContent found within scope ===
    string[] fileLines = ["foo", "bar", "baz", "end"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "X",
            searchContent: "bar\nbaz", scopeStart: 2, scopeEnd: 3);
    assert(res.lines == ["foo", "X", "end"], res.lines.to!string);
    assert(res.matched.matchedAt == 2, res.matched.matchedAt.to!string);
}

unittest {
    // === scope: byContent block outside scope → not found, mentions scope ===
    string[] fileLines = ["foo", "bar", "baz", "end"];
    bool threw = false;
    try {
        editFileUnifiedMemory(fileLines, EditMode.replace, "X", searchContent: "foo\nbar",
                scopeStart: 2, scopeEnd: 4);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("not found"), e.msg);
        assert(e.msg.canFind("within scope [2, 4]"), e.msg);
    }
    assert(threw);
}

unittest {
    // === scope: block anchor in scope may extend past scopeEnd ===
    string[] fileLines = ["foo", "bar", "baz", "qux", "end"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "X",
            searchContent: "bar\nbaz\nqux", scopeStart: 2, scopeEnd: 2);
    assert(res.lines == ["foo", "X", "end"], res.lines.to!string);
    assert(res.matched.matchedAt == 2, res.matched.matchedAt.to!string);
    assert(res.matched.matchedLines == 3, res.matched.matchedLines.to!string);
}

unittest {
    // === scope: scopeEnd beyond EOF clamps to file end ===
    string[] fileLines = ["a", "target", "c"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "x", marker: "target",
            scopeStart: 1, scopeEnd: 100);
    assert(res.lines == ["a", "x", "c"], res.lines.to!string);
    assert(res.matched.matchedAt == 2);
}

unittest {
    // === scope: scopeStart beyond EOF → not found with scope ===
    string[] fileLines = ["a", "b", "c"];
    bool threw = false;
    try {
        editFileUnifiedMemory(fileLines, EditMode.replace, "x", marker: "b",
                scopeStart: 10, scopeEnd: 20);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("not found"), e.msg);
        assert(e.msg.canFind("within scope [10, 20]"), e.msg);
    }
    assert(threw);
}

unittest {
    // === scope: empty file + scope → not found with scope ===
    bool threw = false;
    try {
        editFileUnifiedMemory(cast(string[])[], EditMode.replace, "x", marker: "b",
                scopeStart: 1, scopeEnd: 5);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("not found"), e.msg);
        assert(e.msg.canFind("within scope [1, 5]"), e.msg);
    }
    assert(threw);
}

unittest {
    // === scope: validation error when scopeStart > scopeEnd ===
    bool threw = false;
    try {
        editFileUnifiedMemory(["a"], EditMode.replace, "x", marker: "a",
                scopeStart: 10, scopeEnd: 5);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("scopeStart") && e.msg.canFind("<= scopeEnd"), e.msg);
    }
    assert(threw);
}

unittest {
    // === scope: validation error when scopeStart < 1 ===
    bool threw = false;
    try {
        editFileUnifiedMemory(["a"], EditMode.replace, "x", marker: "a", scopeStart: 0);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("scopeStart 0 must be >= 1"), e.msg);
    }
    assert(threw);
}

unittest {
    // === scope: validation error when scopeEnd < 1 ===
    bool threw = false;
    try {
        editFileUnifiedMemory(["a"], EditMode.replace, "x", marker: "a", scopeEnd: 0);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("scopeEnd 0 must be >= 1"), e.msg);
    }
    assert(threw);
}

unittest {
    // === scope: ignored by byLine targeting ===
    string[] fileLines = ["a", "b", "c"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "x",
            startLine: 2, count: 1, scopeStart: 1, scopeEnd: 1);
    assert(res.lines == ["a", "x", "c"], res.lines.to!string);
    assert(res.matched.matchedAt == 2);
}

unittest {
    // === scope + matchIndex: Nth occurrence within scope ===
    string[] fileLines = ["a", "a", "a", "a"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "x", marker: "a",
            matchIndex: 2, scopeStart: 2, scopeEnd: 4);
    assert(res.lines == ["a", "a", "x", "a"], res.lines.to!string);
    assert(res.matched.matchedAt == 3, res.matched.matchedAt.to!string);
}

unittest {
    // === scope + matchIndex OOB: count only in-scope occurrences ===
    string[] fileLines = ["a", "a", "a", "a"];
    bool threw = false;
    try {
        editFileUnifiedMemory(fileLines, EditMode.replace, "x", marker: "a",
                matchIndex: 4, scopeStart: 2, scopeEnd: 3);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("matchIndex=4") && e.msg.canFind("2 occurrences"), e.msg);
        assert(e.msg.canFind("within scope [2, 3]"), e.msg);
    }
    assert(threw);
}

unittest {
    // === scope + replaceAll: only in-scope occurrences replaced ===
    string[] fileLines = ["a", "a", "a", "a", "a"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "x", marker: "a",
            replaceAll: true, scopeStart: 2, scopeEnd: 3);
    assert(res.lines == ["a", "x", "x", "a", "a"], res.lines.to!string);
    assert(res.matched.matchedAt == 2, res.matched.matchedAt.to!string);
    assert(res.operations == 2, res.operations.to!string);
}

unittest {
    // === scope + replaceAll: no in-scope occurrences → not found with scope ===
    string[] fileLines = ["a", "b", "a"]; // marker only on lines 1 and 3
    bool threw = false;
    try {
        editFileUnifiedMemory(fileLines, EditMode.replace, "x", marker: "a",
                replaceAll: true, scopeStart: 2, scopeEnd: 2);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("not found"), e.msg);
        assert(e.msg.canFind("within scope [2, 2]"), e.msg);
    }
    assert(threw);
}

unittest {
    // === scope + replaceAll byContent: only in-scope blocks replaced ===
    string[] fileLines = ["foo", "bar", "foo", "bar", "foo", "bar", "end"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "X",
            searchContent: "foo\nbar", replaceAll: true, scopeStart: 3, scopeEnd: 5);
    assert(res.lines == ["foo", "bar", "X", "X", "end"], res.lines.to!string);
    assert(res.operations == 2, res.operations.to!string); // anchors at lines 3 and 5
}

unittest {
    // === scope + byContent + replaceAll on empty file → not found ===
    bool threw = false;
    try {
        editFileUnifiedMemory(cast(string[])[], EditMode.replace, "x",
                searchContent: "foo", replaceAll: true, scopeStart: 1, scopeEnd: 5);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("not found"), e.msg);
    }
    assert(threw);
}

unittest {
    // === scope + append mode: anchor in scope, content inserted after ===
    string[] fileLines = ["a", "target", "c", "target2"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.append, "x", marker: "target",
            scopeStart: 2, scopeEnd: 2);
    assert(res.lines == ["a", "target", "x", "c", "target2"], res.lines.to!string);
    assert(res.matched.matchedAt == 2, res.matched.matchedAt.to!string);
}

unittest {
    // === scope + byContent + matchIndex: Nth block within scope ===
    string[] fileLines = ["foo", "bar", "foo", "bar", "foo", "bar", "end"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "X",
            searchContent: "foo\nbar", matchIndex: 2, scopeStart: 3, scopeEnd: 7);
    assert(res.lines == ["foo", "bar", "foo", "bar", "X", "end"], res.lines.to!string);
    assert(res.matched.matchedAt == 5, res.matched.matchedAt.to!string);
    assert(res.matched.matchedLines == 2);
}

unittest {
    // === scope + byMarker + remove: removes only the in-scope line ===
    string[] fileLines = ["a", "marker", "c", "marker", "e"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.remove, "", marker: "marker",
            scopeStart: 2, scopeEnd: 2);
    assert(res.lines == ["a", "c", "marker", "e"], res.lines.to!string);
    assert(res.matched.matchedAt == 2, res.matched.matchedAt.to!string);
    assert(res.linesChanged == -1);
}

unittest {
    // === scope + insert_before: inserts before the in-scope marker ===
    string[] fileLines = ["a", "target", "c", "target2"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.insert_before, "x",
            marker: "target", scopeStart: 2, scopeEnd: 2);
    assert(res.lines == ["a", "x", "target", "c", "target2"], res.lines.to!string);
    assert(res.matched.matchedAt == 2, res.matched.matchedAt.to!string);
}

unittest {
    // === scope: byMarker with count > 1 may extend past scopeEnd (anchor rule)
    // The marker is at line 2 (in scope [2,2]) but targetCount=3 replaces
    // lines 2-4, which reach beyond the scope end. ===
    string[] fileLines = ["a", "target", "c", "d", "e"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "x\ny\nz",
            marker: "target", scopeStart: 2, scopeEnd: 2);
    assert(res.lines == ["a", "x", "y", "z", "e"], res.lines.to!string);
    assert(res.matched.matchedAt == 2, res.matched.matchedAt.to!string);
    assert(res.matched.matchedLines == 3, res.matched.matchedLines.to!string);
}

unittest {
    // === matchIndex with append mode targets the Nth occurrence ===
    string[] fileLines = ["a", "b", "a", "c"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.append, "x", marker: "a", matchIndex: 2);
    assert(res.lines == ["a", "b", "a", "x", "c"], res.lines.to!string);
    assert(res.matched.matchedAt == 3, res.matched.matchedAt.to!string);
}

unittest {
    // === matchIndex=1 (default) targets the first match ===
    string[] fileLines = ["a", "b", "a"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "x", marker: "a", matchIndex: 1);
    assert(res.lines == ["x", "b", "a"], res.lines.to!string);
    assert(res.matched.matchedAt == 1);
}

unittest {
    // === error: remove mode with non-empty content ===
    bool threw = false;
    try {
        editFileUnifiedMemory(["a"], EditMode.remove, "not empty", marker: "a");
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("remove"), e.msg);
    }
    assert(threw);
}

unittest {
    // === error: byLine with count < 1 for replace ===
    bool threw = false;
    try {
        editFileUnifiedMemory(["a", "b"], EditMode.replace, "x", startLine: 1);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("count"), e.msg);
    }
    assert(threw);
}

unittest {
    // === byLine append on an empty file inserts at the start ===
    auto res = editFileUnifiedMemory(cast(string[])[], EditMode.append, "a\nb", startLine: 1);
    assert(res.lines == ["a", "b"], res.lines.to!string);
    assert(res.linesChanged == 2);
}

unittest {
    // === diagnostics: marker closest match ===
    auto diag = buildMarkerDiagnostic(["hello world", "foo bar"], "hello worl");
    assert(diag["closestMatch"]["atLine"].integer == 1, diag.toString);
    assert(diag["closestMatch"]["matchedText"].str == "hello worl", diag.toString);
    assert(diag["searchedLines"].integer == 2, diag.toString);
}

unittest {
    // === diagnostics: block closest match with mismatch detail ===
    auto diag = buildBlockDiagnostic([
        "a", "int total = x + y + z;", "int avg = total / 3;", "b"
    ], "int total = x + y;\nint avg = total / 3;");
    assert(diag["closestMatch"]["atLine"].integer == 2, diag.toString);
    assert(diag["closestMatch"]["mismatchAt"].str.canFind("expected"), diag.toString);
    assert(diag["closestMatch"]["matchedLines"].array.length >= 1, diag.toString);
}

unittest {
    // === diagnostics: marker search respects scope and reports it ===
    auto diag = buildMarkerDiagnostic(["hello world", "foo bar",
        "hello world"], "hello worl", scopeStart: 2, scopeEnd: 3);
    assert(diag["scope"]["start"].integer == 2, diag.toString);
    assert(diag["scope"]["end"].integer == 3, diag.toString);
    assert(diag["searchedLines"].integer == 2, diag.toString);
    assert(diag["closestMatch"]["atLine"].integer == 3, diag.toString);
    // Clamp: scopeEnd beyond EOF reports effective range
    auto diag2 = buildMarkerDiagnostic(["hello world"], "hello worl",
            scopeStart: 1, scopeEnd: 100);
    assert(diag2["scope"]["end"].integer == 1, diag2.toString);
    // Clamp: scopeStart beyond EOF reports an empty effective range
    // (start = one past the last line, end = last line, searchedLines = 0)
    auto diag3 = buildMarkerDiagnostic(["hello world"], "hello worl",
            scopeStart: 10, scopeEnd: 20);
    assert(diag3["scope"]["start"].integer == 2, diag3.toString);
    assert(diag3["scope"]["end"].integer == 1, diag3.toString);
    assert(diag3["searchedLines"].integer == 0, diag3.toString);
}

unittest {
    // === diagnostics: block search respects scope (no match outside scope) ===
    auto diag = buildBlockDiagnostic(["foo", "bar", "baz", "end"], "foo\nbar",
            scopeStart: 3, scopeEnd: 4);
    assert(diag["scope"]["start"].integer == 3, diag.toString);
    assert(diag["searchedLines"].integer == 2, diag.toString);
    // No closestMatch: the anchor "foo" is not inside the scope
    assert(!("closestMatch" in diag), diag.toString);
}

unittest {
    // === byContent + append anchors after the matched block ===
    string[] fileLines = ["a", "foo", "bar", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.append, "X", searchContent: "foo\nbar");
    assert(res.lines == ["a", "foo", "bar", "X", "b"], res.lines.to!string);
    assert(res.matched.matchedAt == 2);
}

unittest {
    // === byContent + insert_after (alias) also anchors after the block ===
    string[] fileLines = ["a", "foo", "bar", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.insert_after, "X",
            searchContent: "foo\nbar");
    assert(res.lines == ["a", "foo", "bar", "X", "b"], res.lines.to!string);
}

unittest {
    // === byContent + append on a block that ends at EOF appends at end ===
    string[] fileLines = ["a", "foo", "bar"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.append, "X", searchContent: "foo\nbar");
    assert(res.lines == ["a", "foo", "bar", "X"], res.lines.to!string);
}

unittest {
    // === byContent replaceAll rejects an explicit count ===
    bool threw = false;
    try {
        editFileUnifiedMemory(["a", "foo", "b"], EditMode.replace, "x",
                searchContent: "foo", count: 2, replaceAll: true);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("count"), e.msg);
    }
    assert(threw);
}

unittest {
    // === autoCountUsed is set for byContent auto-derived counts ===
    string[] fileLines = ["a", "foo", "bar", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "X", searchContent: "foo\nbar");
    assert(res.autoCountUsed, "count derived from block size");
    auto res2 = editFileUnifiedMemory(fileLines, EditMode.replace, "X",
            searchContent: "foo\nbar", count: 2);
    assert(!res2.autoCountUsed, "explicit count");
    auto res3 = editFileUnifiedMemory(fileLines, EditMode.replace, "X",
            searchContent: "foo", replaceAll: true);
    assert(res3.autoCountUsed, "replaceAll uses block size");
}

unittest {
    // === error: explicit startLine 0 is invalid, not "absent" ===
    bool threw = false;
    try {
        editFileUnifiedMemory(["a", "b"], EditMode.replace, "x", startLine: 0);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("startLine") && e.msg.canFind("> 0"), e.msg);
    }
    assert(threw);
}

unittest {
    // === explicit startLine 0 + marker is ambiguous, not ignored ===
    bool threw = false;
    try {
        editFileUnifiedMemory(["a"], EditMode.replace, "x", startLine: 0, marker: "a");
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("ambiguous"), e.msg);
    }
    assert(threw);
}

unittest {
    // === error: matchIndex 0 is invalid ===
    bool threw = false;
    try {
        editFileUnifiedMemory(["a"], EditMode.replace, "x", marker: "a", matchIndex: 0);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("matchIndex") && e.msg.canFind(">= 1"), e.msg);
    }
    assert(threw);
}

unittest {
    // === byLine + remove: deletes the line range ===
    string[] fileLines = ["a", "b", "c", "d", "e"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.remove, "", startLine: 2, count: 2);
    assert(res.lines == ["a", "d", "e"], res.lines.to!string);
    assert(res.matched.matchedAt == 2);
    assert(res.matched.matchedLines == 2);
    assert(res.linesChanged == -2, res.linesChanged.to!string);
    assert(res.operations == 1);
    assert(!res.autoCountUsed);
}

unittest {
    // === byLine + insert_before: content inserted before the line ===
    string[] fileLines = ["a", "b", "c"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.insert_before, "X\nY", startLine: 2);
    assert(res.lines == ["a", "X", "Y", "b", "c"], res.lines.to!string);
    assert(res.linesChanged == 2, res.linesChanged.to!string);
    assert(res.matched.matchedAt == 2);
    assert(res.matched.matchedLines == 1);
}

unittest {
    // === byLine + insert_after (alias): content inserted after the line ===
    string[] fileLines = ["a", "b", "c"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.insert_after, "X", startLine: 2);
    assert(res.lines == ["a", "b", "X", "c"], res.lines.to!string);
    assert(res.linesChanged == 1, res.linesChanged.to!string);
    assert(res.matched.matchedAt == 2);
}

unittest {
    // === byMarker + remove: deletes the marker line ===
    string[] fileLines = ["a", "marker", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.remove, "", marker: "marker");
    assert(res.lines == ["a", "b"], res.lines.to!string);
    assert(res.linesChanged == -1, res.linesChanged.to!string);
    assert(res.matched.matchedAt == 2);
    assert(res.matched.matchedLines == 1);
}

unittest {
    // === byMarker + insert_before: content before the marker line ===
    string[] fileLines = ["a", "marker", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.insert_before, "X", marker: "marker");
    assert(res.lines == ["a", "X", "marker", "b"], res.lines.to!string);
    assert(res.matched.matchedAt == 2);
    assert(res.matched.matchedLines == 1);
}

unittest {
    // === byMarker + insert_after: content after the marker line ===
    string[] fileLines = ["a", "marker", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.insert_after, "X", marker: "marker");
    assert(res.lines == ["a", "marker", "X", "b"], res.lines.to!string);
    assert(res.matched.matchedAt == 2);
    assert(res.matched.matchedLines == 1);
}

unittest {
    // === byMarker with duplicate markers targets the FIRST by default ===
    string[] fileLines = ["x", "marker", "marker", "z"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "X", marker: "marker");
    assert(res.lines == ["x", "X", "marker", "z"], res.lines.to!string);
    assert(res.matched.matchedAt == 2, res.matched.matchedAt.to!string);
}

unittest {
    // === byContent with duplicate blocks targets the FIRST by default ===
    string[] fileLines = ["a", "foo", "b", "foo", "c"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "X", searchContent: "foo");
    assert(res.lines == ["a", "X", "b", "foo", "c"], res.lines.to!string);
    assert(res.matched.matchedAt == 2, res.matched.matchedAt.to!string);
}

unittest {
    // === empty file + byMarker → not found error ===
    bool threw = false;
    try {
        editFileUnifiedMemory(cast(string[])[], EditMode.replace, "x", marker: "m");
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("not found"), e.msg);
    }
    assert(threw);
}

unittest {
    // === empty file + byContent → not found error ===
    bool threw = false;
    try {
        editFileUnifiedMemory(cast(string[])[], EditMode.replace, "x", searchContent: "m");
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("not found"), e.msg);
    }
    assert(threw);
}

unittest {
    // === empty file + byLine insert_before creates content at the start ===
    auto res = editFileUnifiedMemory(cast(string[])[], EditMode.insert_before, "a", startLine: 1);
    assert(res.lines == ["a"], res.lines.to!string);
    assert(res.linesChanged == 1, res.linesChanged.to!string);
}

unittest {
    // === single-line file + byMarker replace with single-line content ===
    auto res = editFileUnifiedMemory(["only"], EditMode.replace, "X", marker: "only");
    assert(res.lines == ["X"], res.lines.to!string);
    assert(!res.autoCountUsed);
    assert(res.matched.matchedAt == 1);
}

unittest {
    // === single-line file + byMarker replace with multi-line content:
    // auto-count would exceed the file → clear error ===
    bool threw = false;
    try {
        editFileUnifiedMemory(["only"], EditMode.replace, "X\nY", marker: "only");
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("exceeds file length"), e.msg);
    }
    assert(threw, "auto-count beyond EOF on a single-line file must error");
}

unittest {
    // === single-line file + byContent replace ===
    auto res = editFileUnifiedMemory(["only"], EditMode.replace, "X", searchContent: "only");
    assert(res.lines == ["X"], res.lines.to!string);
    assert(res.matched.matchedAt == 1);
    assert(res.matched.matchedLines == 1);
}

unittest {
    // === single-line file + byLine remove clears the file ===
    auto res = editFileUnifiedMemory(["only"], EditMode.remove, "", startLine: 1, count: 1);
    assert(res.lines.length == 0, res.lines.to!string);
    assert(res.linesChanged == -1, res.linesChanged.to!string);
}

unittest {
    // === multi-line content preserved exactly through the unified path ===
    // (no indentation or whitespace is added or removed)
    string[] fileLines = ["a", "marker", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace,
            "  keep\tindent\n\t\ttab", marker: "marker", count: 1);
    assert(res.lines == ["a", "  keep\tindent", "\t\ttab", "b"], res.lines.to!string);
    assert(!res.autoCountUsed);
}

unittest {
    // === byContent + append + explicit count still anchors after the WHOLE block ===
    // (count is ignored for insert modes; the anchor comes from the block only)
    string[] fileLines = ["a", "foo", "bar", "baz", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.append, "X",
            searchContent: "foo\nbar\nbaz", count: 1);
    assert(res.lines == ["a", "foo", "bar", "baz", "X", "b"], res.lines.to!string);
    assert(res.matched.matchedLines == 3, res.matched.matchedLines.to!string);
}

unittest {
    // === byContent + insert_before + explicit count anchors before the block ===
    string[] fileLines = ["a", "foo", "bar", "baz", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.insert_before, "X",
            searchContent: "foo\nbar\nbaz", count: 1);
    assert(res.lines == ["a", "X", "foo", "bar", "baz", "b"], res.lines.to!string);
    assert(res.matched.matchedLines == 3, res.matched.matchedLines.to!string);
}

unittest {
    // === byMarker insert modes ignore an explicit count ===
    string[] fileLines = ["a", "marker", "b", "c"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.append, "X", marker: "marker", count: 3);
    assert(res.lines == ["a", "marker", "X", "b", "c"], res.lines.to!string);
    assert(res.matched.matchedLines == 1, res.matched.matchedLines.to!string);
}

unittest {
    // === byLine insert modes also ignore an explicit count ===
    string[] fileLines = ["a", "b", "c"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.insert_after, "X",
            startLine: 2, count: 3);
    assert(res.lines == ["a", "b", "X", "c"], res.lines.to!string);
    assert(res.matched.matchedLines == 1);
}

unittest {
    // Multi-hunk test: lines between hunks must be preserved
    // This reproduces the bug where fileLineIdx is reset at each hunk,
    // skipping lines between hunks
    auto content2 = [
        `writeln("Line 1");`, `writeln("Line 2");`, `writeln("Line 3");`,
        `writeln("Line 4");`, `writeln("Line 5");`, `writeln("Line 6");`,
        `writeln("Line 7");`, `writeln("Line 8");`, `writeln("Line 9");`,
        `writeln("Line 10");`
    ];

    // Hunk 1: modify only line 4 (1 line)
    // Hunk 2: modify only line 8 (1 line)
    // Lines 5, 6, 7 are between hunks and must be preserved
    auto diff2 = [
        "--- old.txt", "+++ new.txt", "@@ -4,1 +4,1 @@", `-writeln("Line 4");`,
        `+writeln("Line 44");`, "@@ -8,1 +8,1 @@", `-writeln("Line 8");`,
        `+writeln("Line 88");`,
    ];

    auto result2 = applyDiffMemory(content2, diff2);

    // Expected: all 10 lines with lines 4 and 8 modified
    assert(result2.lines.length == 10,
            "Multi-hunk: expected 10 lines but got " ~ result2.lines.length.to!string);
    assert(result2.lines[0] == `writeln("Line 1");`);
    assert(result2.lines[1] == `writeln("Line 2");`);
    assert(result2.lines[2] == `writeln("Line 3");`);
    assert(result2.lines[3] == `writeln("Line 44");`);
    assert(result2.lines[4] == `writeln("Line 5");`);
    assert(result2.lines[5] == `writeln("Line 6");`);
    assert(result2.lines[6] == `writeln("Line 7");`);
    assert(result2.lines[7] == `writeln("Line 88");`);
    assert(result2.lines[8] == `writeln("Line 9");`);
    assert(result2.lines[9] == `writeln("Line 10");`);
    assert(result2.warnings.empty, "Multi-hunk: correct counts should produce no warnings");
    assert(result2.hunksApplied == 2, "Multi-hunk: expected 2 hunks applied");
}

unittest {
    // dfmt off
    auto content = [
        "int main() {",
        "    int a = 1;",
        "    int b = 2;",
        "    int c = 3;",
        "    return 0;",
        "}"
    ];

    // Patch from @@ -1,6 +1,6 @@ — only one line changed, rest is context
    auto diff = [
        "--- a/test.c",
        "+++ b/test.c",
        "@@ -1,6 +1,6 @@",
        " int main() {",
        "-    int a = 1;",
        "+    int a = 10;",
        "     int b = 2;",
        "     int c = 3;",
        "     return 0;",
        " }",
    ];
    // dfmt on

    auto result = applyDiffMemory(content, diff);
    assert(result.lines.length == 6,
            "Context+additions: expected 6 lines but got " ~ result.lines.length.to!string);
    assert(result.lines[0] == "int main() {");
    assert(result.lines[1] == "    int a = 10;");
    assert(result.lines[2] == "    int b = 2;");
    assert(result.lines[3] == "    int c = 3;");
    assert(result.lines[4] == "    return 0;");
    assert(result.lines[5] == "}");
    assert(result.warnings.empty, "correct counts should produce no warnings");
}

unittest {
    // dfmt off
    auto content = [
        "int main() {",
        "    int a = 1;",
        "    int b = 2;",
        "    int c = 3;",
        "    return 0;",
        "}"
    ];

    // Remove one line, rest is context
    auto diff = [
        "--- a/test.c",
        "+++ b/test.c",
        "@@ -1,6 +1,5 @@",
        " int main() {",
        "-    int a = 1;",
        "     int b = 2;",
        "     int c = 3;",
        "     return 0;",
        " }",
    ];
    // dfmt on

    auto result = applyDiffMemory(content, diff);
    assert(result.lines.length == 5,
            "Context+removals: expected 5 lines but got " ~ result.lines.length.to!string);
    assert(result.lines[0] == "int main() {");
    assert(result.lines[1] == "    int b = 2;");
    assert(result.lines[2] == "    int c = 3;");
    assert(result.lines[3] == "    return 0;");
    assert(result.lines[4] == "}");
    assert(result.warnings.empty, "correct counts should produce no warnings");
}

unittest {
    auto content = ["line1", "line2", "line3",];

    auto diff = [
        "--- a/test.txt", "+++ b/test.txt", "@@ -1,3 +1,3 @@", " line1", " line2",
        " line3",
    ];

    auto result = applyDiffMemory(content, diff);
    assert(result.lines.length == 3,
            "Context-only: expected 3 lines but got " ~ result.lines.length.to!string);
    assert(result.lines[0] == "line1");
    assert(result.lines[1] == "line2");
    assert(result.lines[2] == "line3");
    assert(result.warnings.empty, "correct counts should produce no warnings");
}

unittest {
    // Test: deletion only (no context lines)
    auto content3 = [`Line A`, `Line B`, `Line C`, `Line D`, `Line E`];
    auto diff3 = ["--- old.txt", "+++ new.txt", "@@ -3,1 +2,0 @@", `-Line C`];
    auto result3 = applyDiffMemory(content3, diff3);
    assert(result3.lines.length == 4,
            "Delete: expected 4 lines but got " ~ result3.lines.length.to!string);
    assert(result3.lines[0] == `Line A`);
    assert(result3.lines[1] == `Line B`, result3.lines[1]);
    assert(result3.lines[2] == `Line D`);
    assert(result3.lines[3] == `Line E`);
    assert(result3.warnings.empty, "correct counts should produce no warnings");
}

unittest {
    // Removal lines past EOF must error instead of silently "succeeding".
    auto content = ["only one line"];
    auto diff = [
        "--- a.txt", "+++ b.txt", "@@ -1,2 +1,0 @@", "-only one line", "-phantom"
    ];
    bool threw = false;
    try {
        applyDiffMemory(content, diff);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("Unexpected end of file"), e.msg);
    }
    assert(threw, "removal past EOF must throw");
}

unittest {
    // Test: multiple additions in one hunk
    auto content4 = [`A`, `B`, `C`];
    auto diff4 = [
        "--- old.txt", "+++ new.txt", "@@ -1,1 +1,3 @@", `-A`, `+A1`, `+A2`, `+A3`,
    ];
    auto result4 = applyDiffMemory(content4, diff4);
    assert(result4.lines.length == 5,
            "Additions: expected 5 lines but got " ~ result4.lines.length.to!string);
    assert(result4.lines[0] == `A1`);
    assert(result4.lines[1] == `A2`);
    assert(result4.lines[2] == `A3`);
    assert(result4.lines[3] == `B`);
    assert(result4.lines[4] == `C`);
    assert(result4.warnings.empty, "correct counts should produce no warnings");
}

unittest {
    // Test: hunk header declares 3 old lines, but only 2 removal lines are present.
    // This simulates an LLM that forgets a context or deletion line.
    // Header counts are advisory: the diff applies with a warning.
    auto content = ["line1", "line2", "line3", "line4", "line5"];

    auto diff = [
        "--- a.txt", "+++ b.txt", "@@ -2,3 +2,2 @@", // declares 3 old lines (line2, line3, line4)
        "-line2", // only two '-' lines supplied
        "-line3", "+newline", "@@ -5,1 +5,1 @@", " line5"
    ];

    auto res = applyDiffMemory(content, diff);
    assert(res.warnings.length == 2, "expected 2 warnings but got: " ~ res.warnings.to!string);
    assert(res.warnings[0].canFind("Hunk 1") && res.warnings[0].canFind("old lines"),
            "warning must mention hunk 1 and old lines: " ~ res.warnings[0]);
    assert(res.warnings[1].canFind("Hunk 1") && res.warnings[1].canFind("new lines"),
            "warning must mention hunk 1 and new lines: " ~ res.warnings[1]);
    assert(res.lines.length == 4, "diff should still apply: " ~ res.lines.to!string);
    assert(res.hunksApplied == 2, "both hunks should be applied");
}

unittest {
    // Auto-count: header declares 5 old lines but body has 6 → warning, uses body count
    auto content = ["a", "b", "c", "d", "e", "f"];
    auto diff = [
        "--- a.txt", "+++ b.txt", "@@ -1,5 +1,1 @@", "-a", "-b", "-c", "-d", "-e",
        "-f", "+X",
    ];
    auto res = applyDiffMemory(content, diff);
    assert(res.lines.length == 1, "expected 1 line but got " ~ res.lines.to!string);
    assert(res.lines[0] == "X", res.lines.to!string);
    assert(res.warnings.length == 1, "expected 1 warning: " ~ res.warnings.to!string);
    assert(res.warnings[0].canFind("declared 5 old lines, body has 6"),
            "warning must report discrepancy: " ~ res.warnings[0]);
    assert(res.hunksApplied == 1);
}

unittest {
    // Auto-count: header declares 3 new lines but body has 4 → warning, uses body count
    auto content = ["a", "b", "c"];
    auto diff = [
        "--- a.txt", "+++ b.txt", "@@ -1,1 +1,3 @@", "-a", "+A1", "+A2", "+A3",
        "+A4",
    ];
    auto res = applyDiffMemory(content, diff);
    assert(res.lines.length == 6, "expected 6 lines but got " ~ res.lines.to!string);
    assert(res.lines[0] == "A1" && res.lines[1] == "A2" && res.lines[2] == "A3"
            && res.lines[3] == "A4" && res.lines[4] == "b" && res.lines[5] == "c",
            res.lines.to!string);
    assert(res.warnings.length == 1, "expected 1 warning: " ~ res.warnings.to!string);
    assert(res.warnings[0].canFind("declared 3 new lines, body has 4"),
            "warning must report discrepancy: " ~ res.warnings[0]);
    assert(res.hunksApplied == 1);
}

unittest {
    // Multi-hunk: mix of correct and wrong counts — all hunks apply, warnings per hunk
    auto content = ["l1", "l2", "l3", "l4", "l5", "l6", "l7"];
    auto diff = [
        "--- a.txt", "+++ b.txt", "@@ -1,2 +1,1 @@", // wrong: body has 1 old and 2 new
        "-l1", "+A", "+B",
        "@@ -4,1 +4,1 @@", // correct
        "-l4", "+D", "@@ -6,2 +6,2 @@", // wrong: body has 1 old and 1 new
        "-l6", "+F",
    ];
    auto res = applyDiffMemory(content, diff);
    assert(res.lines.length == 8, "expected 8 lines but got " ~ res.lines.to!string);
    assert(res.lines == ["A", "B", "l2", "l3", "D", "l5", "F", "l7"], res.lines.to!string);
    assert(res.warnings.length == 4, "expected 4 warnings but got: " ~ res.warnings.to!string);
    assert(res.warnings[0].canFind("Hunk 1")
            && res.warnings[0].canFind("old lines"), res.warnings[0]);
    assert(res.warnings[1].canFind("Hunk 1")
            && res.warnings[1].canFind("new lines"), res.warnings[1]);
    assert(res.warnings[2].canFind("Hunk 3")
            && res.warnings[2].canFind("old lines"), res.warnings[2]);
    assert(res.warnings[3].canFind("Hunk 3")
            && res.warnings[3].canFind("new lines"), res.warnings[3]);
    assert(res.hunksApplied == 3, "all hunks should be applied");
}

unittest {
    // Fuzzy matching (default): context line with extra leading space matches
    // a file line with different indentation.
    auto content = ["    int a = 1;", "    int b = 2;"];
    auto diff = [
        "--- a.txt", "+++ b.txt", "@@ -1,2 +1,2 @@", "  int a = 1;", // diff context has extra leading space vs file line
        "     int b = 2;", // extra leading space
    ];
    auto res = applyDiffMemory(content, diff); // fuzzy defaults to true
    assert(res.lines.length == 2, res.lines.to!string);
    assert(res.lines[0] == "    int a = 1;", res.lines[0]);
    assert(res.lines[1] == "    int b = 2;", res.lines[1]);
    assert(res.warnings.empty, res.warnings.to!string);
    assert(res.hunksApplied == 1);
}

unittest {
    // Fuzzy matching: context line with trailing whitespace matches.
    auto content = ["alpha", "beta", "gamma"];
    auto diff = [
        "--- a.txt", "+++ b.txt", "@@ -1,3 +1,3 @@", " alpha ", // trailing space in diff context line
        " beta", " gamma",
    ];
    auto res = applyDiffMemory(content, diff);
    assert(res.lines.length == 3, res.lines.to!string);
    assert(res.lines[0] == "alpha", res.lines[0]);
    assert(res.lines[1] == "beta", res.lines[1]);
    assert(res.lines[2] == "gamma", res.lines[2]);
    assert(res.warnings.empty, res.warnings.to!string);
}

unittest {
    // Fuzzy matching must NOT change what is written: file lines are preserved
    // verbatim (only the matching is fuzzy). '+' lines are written as-is.
    auto content = ["  original indented", "keep"];
    auto diff = [
        "--- a.txt", "+++ b.txt", "@@ -1,2 +1,3 @@", " original indented", // diff context has 1 space, file has 2
        " keep", "+  ADDED  ",
    ];
    auto res = applyDiffMemory(content, diff);
    assert(res.lines.length == 3, res.lines.to!string);
    assert(res.lines[0] == "  original indented",
            "file line must be preserved verbatim: " ~ res.lines[0]);
    assert(res.lines[1] == "keep");
    assert(res.lines[2] == "  ADDED  ",
            "add line must be written exactly as provided: " ~ res.lines[2]);
}

unittest {
    // fuzzy=false: whitespace difference causes context mismatch error.
    auto content = ["    int a = 1;", "    int b = 2;"];
    auto diff = [
        "--- a.txt", "+++ b.txt", "@@ -1,2 +1,2 @@", "  int a = 1;", // extra leading space vs file line
        "     int b = 2;",
    ];
    bool threw = false;
    try {
        applyDiffMemory(content, diff, false);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("Context mismatch"), e.msg);
        assert(e.msg.canFind("line 1"), e.msg);
    }
    assert(threw, "fuzzy=false must reject whitespace differences in context lines");
}

unittest {
    // fuzzy=false with exact matches still applies normally.
    auto content = ["line1", "line2", "line3"];
    auto diff = [
        "--- a.txt", "+++ b.txt", "@@ -1,3 +1,3 @@", " line1", "-line2", "+LINE2",
        " line3",
    ];
    auto res = applyDiffMemory(content, diff, false);
    assert(res.lines == ["line1", "LINE2", "line3"], res.lines.to!string);
    assert(res.warnings.empty, res.warnings.to!string);
    assert(res.hunksApplied == 1);
}

unittest {
    // Default (fuzzy=true) tolerates whitespace differences in a real edit:
    // context lines matched fuzzily, replacement applied.
    auto content = ["void foo() {", "    bar(1);", "    bar(2);", "}"];
    auto diff = [
        "--- a.c", "+++ b.c", "@@ -1,4 +1,4 @@", " void foo() {",
        "-   bar(1);", // file has 4 spaces, diff has 3
        "+   bar(10);", "     bar(2);", " }",
    ];
    auto res = applyDiffMemory(content, diff);
    assert(res.lines == ["void foo() {", "   bar(10);", "    bar(2);", "}"], res.lines.to!string);
    assert(res.warnings.empty, res.warnings.to!string);
}

unittest {
    // Integration test: editFile byLine - replace mode with metadata
    auto ctx = makeTestContext("edit_byline_replace");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "a\nb\nc\nd\ne\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "X\nY", mode: "replace", startLine: 2, count: 2));
    assert(result.success, result.msg);

    auto json = parseJSON(result.msg);
    assert(json["ok"].boolean);
    assert(json["matchedAt"].integer == 2, result.msg);
    assert(json["matchedLines"].integer == 2, result.msg);
    assert(json["linesChanged"].integer == 0, result.msg);
    assert(json["operations"].integer == 1, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "a\nX\nY\nd\ne\n", content);
}

unittest {
    // Integration test: editFile byMarker - replace mode (single-line content)
    auto ctx = makeTestContext("edit_marker_replace");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nhello world\nline3\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "replaced", mode: "replace", marker: "hello world"));
    assert(result.success, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "line1\nreplaced\nline3\n", content);
}

unittest {
    // Integration test: editFile byMarker - remove mode
    auto ctx = makeTestContext("edit_marker_remove");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nhello world\nline3\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "", mode: "remove", marker: "hello world"));
    assert(result.success, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "line1\nline3\n", content);
}

unittest {
    // Integration test: editFile byMarker - insert_before mode
    auto ctx = makeTestContext("edit_marker_insertbefore");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nhello world\nline3\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "inserted", mode: "insert-before", marker: "hello world"));
    assert(result.success, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "line1\ninserted\nhello world\nline3\n", content);
}

unittest {
    // Integration test: editFile byMarker - insert_after mode (alias for append)
    auto ctx = makeTestContext("edit_marker_insertafter");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nhello world\nline3\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "inserted", mode: "insert-after", marker: "hello world"));
    assert(result.success, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "line1\nhello world\ninserted\nline3\n", content);
}

unittest {
    // Integration test: editFile byMarker - auto-count with multi-line content:
    // 3-line content replaces the marker line plus the next 2 lines.
    auto ctx = makeTestContext("edit_marker_autocount");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nmarker\nline3\nline4\nline5\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "newA\nnewB\nnewC", mode: "replace", marker: "marker"));
    assert(result.success, result.msg);

    auto json = parseJSON(result.msg);
    assert(json["matchedLines"].integer == 3, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "line1\nnewA\nnewB\nnewC\nline5\n", content);
}

unittest {
    // Integration test: editFile byMarker - explicit count=1 keeps the marker
    // line as the only replaced line even with multi-line content.
    auto ctx = makeTestContext("edit_marker_count1");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nmarker\nline3\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt", content: "newA\nnewB\nnewC",
            mode: "replace", marker: "marker", count: 1));
    assert(result.success, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "line1\nnewA\nnewB\nnewC\nline3\n", content);
}

unittest {
    // Integration test: editFile byMarker - auto-count exceeding file length
    // must fail with a clear error (the old tool silently misbehaved here).
    auto ctx = makeTestContext("edit_marker_autocount_oob");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nmarker\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "newA\nnewB\nnewC", mode: "replace", marker: "marker"));
    assert(!result.success, result.msg);
    assert(result.msg.canFind("exceeds file length"), result.msg);
}

unittest {
    // Integration test: editFile byMarker - error: marker not found with diagnostic
    auto ctx = makeTestContext("edit_marker_notfound");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nline2\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "x", mode: "replace", marker: "notfound"));
    assert(!result.success);
    assert(result.msg.canFind("not found"), result.msg);

    auto json = parseJSON(result.msg);
    assert(!json["ok"].boolean);
    assert(json["diagnostic"]["searchedLines"].integer == 2, result.msg);
}

unittest {
    // Integration test: editFile - error: empty marker means no targeting
    auto ctx = makeTestContext("edit_marker_empty");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "x", mode: "replace", marker: ""));
    assert(!result.success);
    assert(result.msg.canFind("targeting"), result.msg);
}

unittest {
    // Integration test: editFile - error: ambiguous targeting
    auto ctx = makeTestContext("edit_ambiguous");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "x", mode: "replace", startLine: 1, marker: "line1"));
    assert(!result.success);
    assert(result.msg.canFind("ambiguous"), result.msg);
}

unittest {
    // Integration test: editFile byContent - single line replace (first match only)
    auto ctx = makeTestContext("edit_content_single");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nfunction foo() {\n    return 1;\n}\nline5\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt", content: "function bar() {\n    return 2;\n}",
            mode: "replace", searchContent: "function foo() {\n    return 1;\n}"));
    assert(result.success, result.msg);

    auto json = parseJSON(result.msg);
    assert(json["matchedAt"].integer == 2, result.msg);
    assert(json["matchedLines"].integer == 3, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content.canFind("function bar()"), content);
    assert(content.canFind("return 2"), content);
    assert(!content.canFind("function foo()"), content);
}

unittest {
    // Integration test: editFile byContent - trimmed equality (whitespace tolerance)
    auto ctx = makeTestContext("edit_content_trimmed");
    scope (exit)
        ctx.teardown;

    // File has 4-space indentation
    createTestFile(ctx, "test.txt", "    int x = 1;\n    int y = 2;\n");

    // Search with tab indentation - should still match via trimmed equality
    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt", content: "    int x = 100;",
            mode: "replace", searchContent: "\tint x = 1;"));
    assert(result.success, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content.canFind("int x = 100"), content);
}

unittest {
    // Integration test: editFile byContent - error: search block not found
    auto ctx = makeTestContext("edit_content_notfound");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nline2\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "replacement", mode: "replace", searchContent: "notfound"));
    assert(!result.success);
    assert(result.msg.canFind("not found"), result.msg);

    auto json = parseJSON(result.msg);
    assert(!json["ok"].boolean);
    assert(json["diagnostic"]["searchLines"].array.length == 1, result.msg);
}

unittest {
    // Integration test: editFile - error: empty searchContent means no targeting
    auto ctx = makeTestContext("edit_content_empty");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "replacement", mode: "replace", searchContent: ""));
    assert(!result.success);
    assert(result.msg.canFind("targeting"), result.msg);
}

unittest {
    // Integration test: editFile byContent replaceAll - multiple replacements
    auto ctx = makeTestContext("edit_content_all");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "foo\nbar\nfoo\nbaz\nfoo\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt", content: "replaced",
            mode: "replace", searchContent: "foo", replaceAll: true));
    assert(result.success, result.msg);

    auto json = parseJSON(result.msg);
    assert(json["operations"].integer == 3, json["operations"].integer.to!string);
    assert(json["matchedAt"].integer == 1, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "replaced\nbar\nreplaced\nbaz\nreplaced\n", content);
}

unittest {
    // Integration test: editFile byMarker replaceAll - each marker line replaced
    auto ctx = makeTestContext("edit_marker_all");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "a\nmarker\nb\nmarker\nc\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "X", mode: "replace", marker: "marker", replaceAll: true));
    assert(result.success, result.msg);

    auto json = parseJSON(result.msg);
    assert(json["operations"].integer == 2, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "a\nX\nb\nX\nc\n", content);
}

unittest {
    // Integration test: editFile byContent replaceAll - error: zero matches
    auto ctx = makeTestContext("edit_content_all_zero");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nline2\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt", content: "replacement",
            mode: "replace", searchContent: "notfound", replaceAll: true));
    assert(!result.success);
    assert(result.msg.canFind("not found"), result.msg);
}

unittest {
    // Integration test: editFile replaceAll with byLine - not supported
    auto ctx = makeTestContext("edit_byline_all");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "x", mode: "replace", startLine: 1, count: 1, replaceAll: true));
    assert(!result.success);
    assert(result.msg.canFind("replaceAll"), result.msg);
}

unittest {
    // Integration test: editFile dryRun - does NOT modify file, returns preview
    auto ctx = makeTestContext("edit_dryrun");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nhello world\nline3\n");

    string original = readTestFile(ctx, "test.txt");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt", content: "replaced",
            mode: "replace", marker: "hello world", dryRun: true));
    assert(result.success, result.msg);

    // Verify file was NOT modified
    auto content = readTestFile(ctx, "test.txt");
    assert(content == original, content);

    // Verify preview contains modified content
    auto json = parseJSON(result.msg);
    assert(json["preview"].str.canFind("replaced"), result.msg);
    assert(json["matchedAt"].integer == 2, result.msg);
}

unittest {
    // Integration test: editFile dryRun with multi-line content and explicit
    // count=1 - preview shows all inserted lines, file untouched.
    auto ctx = makeTestContext("edit_dryrun_fields");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nhello world\nline3\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt", content: "new1\nnew2",
            mode: "replace", marker: "hello world", count: 1, dryRun: true));
    assert(result.success, result.msg);

    auto json = parseJSON(result.msg);
    assert(json["preview"].str.canFind("new1"), result.msg);
    assert(json["preview"].str.canFind("new2"), result.msg);
    assert(json["matchedAt"].integer == 2, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "line1\nhello world\nline3\n", content);
}

unittest {
    // Integration test: editFile byLine dryRun - preview without modifying file
    auto ctx = makeTestContext("edit_byline_dryrun");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "a\nb\nc\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "X", mode: "replace", startLine: 2, count: 1, dryRun: true));
    assert(result.success, result.msg);

    auto json = parseJSON(result.msg);
    assert(json["preview"].str.canFind("X"), result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "a\nb\nc\n", content);
}

unittest {
    // Integration test: editFile byLine append - content added after the line
    auto ctx = makeTestContext("edit_byline_append");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "a\nb\nc\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "X", mode: "append", startLine: 2));
    assert(result.success, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "a\nb\nX\nc\n", content);
}

unittest {
    // Integration test: editFile byContent with explicit count extends the range
    auto ctx = makeTestContext("edit_content_count");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "1\nfoo\nbar\nbaz\n5\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "X", mode: "replace", searchContent: "foo", count: 3));
    assert(result.success, result.msg);

    auto json = parseJSON(result.msg);
    assert(json["matchedLines"].integer == 3, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "1\nX\n5\n", content);
}

unittest {
    // Integration test: matchIndex=2 targets the SECOND occurrence (byMarker)
    auto ctx = makeTestContext("edit_matchindex");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "a\na\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "x", mode: "replace", marker: "a", matchIndex: 2));
    assert(result.success, result.msg);

    auto json = parseJSON(result.msg);
    assert(json["matchedAt"].integer == 2, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "a\nx\n", content);
}

unittest {
    // Integration test: matchIndex OOB reports the actual occurrence count
    auto ctx = makeTestContext("edit_matchindex_oob");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "a\nb\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "x", mode: "replace", marker: "a", matchIndex: 3));
    assert(!result.success);
    assert(result.msg.canFind("matchIndex=3") && result.msg.canFind("1 occurrence"), result.msg);

    // The failure JSON carries the diagnostic field promised by the tool
    // description (closest-match info for the marker) plus a suggestion.
    auto json = parseJSON(result.msg);
    assert(json["ok"].type == JSONType.false_, result.msg);
    assert(json["diagnostic"].type == JSONType.object, result.msg);
    assert(json["suggestion"].type == JSONType.string, result.msg);
}

unittest {
    // Integration test: matchIndex OOB with byContent reports the actual
    // occurrence count in the tool-level JSON error.
    auto ctx = makeTestContext("edit_matchindex_content_oob");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "foo\nbar\nfoo\nend\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "x", mode: "replace", searchContent: "foo\nbar", matchIndex: 3));
    assert(!result.success);
    assert(result.msg.canFind("matchIndex=3") && result.msg.canFind("1 occurrence"), result.msg);

    auto json = parseJSON(result.msg);
    assert(json["ok"].type == JSONType.false_, result.msg);
    assert(json["diagnostic"].type == JSONType.object, result.msg);
    assert(json["suggestion"].type == JSONType.string, result.msg);
}

unittest {
    // Integration test: matchIndex > 1 with replaceAll is rejected
    auto ctx = makeTestContext("edit_matchindex_replaceall");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "a\na\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt", content: "x",
            mode: "replace", marker: "a", replaceAll: true, matchIndex: 2));
    assert(!result.success);
    assert(result.msg.canFind("matchIndex") && result.msg.canFind("replaceAll"), result.msg);
}

unittest {
    // Integration test: matchIndex=2 targets the second block (byContent)
    auto ctx = makeTestContext("edit_matchindex_content");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "foo\nbar\nfoo\nbar\nend\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "X", mode: "replace", searchContent: "foo\nbar", matchIndex: 2));
    assert(result.success, result.msg);

    auto json = parseJSON(result.msg);
    assert(json["matchedAt"].integer == 3, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "foo\nbar\nX\nend\n", content);
}

unittest {
    // Integration test: matchIndex is ignored with byLine targeting
    auto ctx = makeTestContext("edit_matchindex_bline");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "a\nb\nc\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "x", mode: "replace", startLine: 2, count: 1, matchIndex: 2));
    assert(result.success, result.msg);

    auto json = parseJSON(result.msg);
    assert(json["matchedAt"].integer == 2, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "a\nx\nc\n", content);
}

unittest {
    // Integration test: byMarker targets the FIRST of duplicate markers
    auto ctx = makeTestContext("edit_marker_duplicates");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "x\nmarker\nmarker\nz\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "X", mode: "replace", marker: "marker"));
    assert(result.success, result.msg);

    auto json = parseJSON(result.msg);
    assert(json["matchedAt"].integer == 2, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "x\nX\nmarker\nz\n", content);
}

unittest {
    // Integration test: empty file - byLine append creates content and
    // preserves the original no-trailing-newline state.
    auto ctx = makeTestContext("edit_empty_append");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "a\nb", mode: "append", startLine: 1));
    assert(result.success, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "a\nb", content);
}

unittest {
    // Integration test: single-line file - replace
    auto ctx = makeTestContext("edit_single_line");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "only\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "X", mode: "replace", startLine: 1, count: 1));
    assert(result.success, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "X\n", content);
}

unittest {
    // Integration test: trailing-newline state preserved; dryRun preview
    // matches the exact bytes that would be written.
    auto ctx = makeTestContext("edit_no_trailing_nl");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "a\nb"); // NO trailing newline

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "X", mode: "replace", startLine: 2, count: 1, dryRun: true));
    assert(result.success, result.msg);
    auto json = parseJSON(result.msg);
    assert(json["preview"].str == "a\nX", json["preview"].str);

    result = editFile(ctx, UnifiedEditFileParams(path: "test.txt", content: "X",
            mode: "replace", startLine: 2, count: 1));
    assert(result.success, result.msg);
    auto content = readTestFile(ctx, "test.txt");
    assert(content == "a\nX", content);
}

unittest {
    // Integration test: byContent append inserts after the whole matched block
    auto ctx = makeTestContext("edit_content_append");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "a\nfoo\nbar\nb\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "X", mode: "append", searchContent: "foo\nbar"));
    assert(result.success, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "a\nfoo\nbar\nX\nb\n", content);
}

unittest {
    // Integration test: byContent replaceAll with explicit count is rejected
    auto ctx = makeTestContext("edit_content_all_count");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "foo\nbar\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt", content: "x",
            mode: "replace", searchContent: "foo", count: 2, replaceAll: true));
    assert(!result.success);
    assert(result.msg.canFind("count"), result.msg);
}

unittest {
    // Integration test: matchIndex 0 is invalid
    auto ctx = makeTestContext("edit_matchindex_zero");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "a\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "x", mode: "replace", marker: "a", matchIndex: 0));
    assert(!result.success);
    assert(result.msg.canFind("matchIndex"), result.msg);
}

unittest {
    // Integration test: applyDiff fuzzy matching with dryRun — whitespace
    // differences in context lines are tolerated (default fuzzy=true),
    // file is not modified, preview shows the result.
    auto ctx = makeTestContext("difffuzzy");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "  indented line1\n  indented line2\n  indented line3\n");

    string original = readTestFile(ctx, "test.txt");

    // Diff context lines have different leading whitespace than the file lines.
    string diff = "--- a/test.txt\n+++ b/test.txt\n@@ -1,3 +1,3 @@\n indented line1\n-  indented line2\n+  INDENTED line2\n indented line3\n";

    auto result = applyDiff(ctx, ApplyDiffParams("test.txt", diff, dryRun: true));
    assert(result.success, result.msg);

    // File must NOT be modified in dry-run mode
    assert(readTestFile(ctx, "test.txt") == original, "dryRun must not modify the file");

    auto json = parseJSON(result.msg);
    assert(json["preview"].str.canFind("INDENTED line2"), json.toString);
    assert(json["warnings"].array.empty, json.toString);
    assert(json["hunksApplied"].integer == 1, json.toString);
}

unittest {
    // Integration test: applyDiff fuzzy=false — whitespace difference in a
    // context line fails with a diagnostic error.
    auto ctx = makeTestContext("difffuzzy_exact");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "  indented line1\n  indented line2\n");

    string diff = "--- a/test.txt\n+++ b/test.txt\n@@ -1,2 +1,2 @@\n indented line1\n indented line2\n";

    auto result = applyDiff(ctx, ApplyDiffParams("test.txt", diff, fuzzy: false));
    assert(!result.success, result.msg);
    assert(result.msg.canFind("Context mismatch"), result.msg);
}

unittest {
    // Integration test: applyDiffDryRun - does NOT modify file
    auto ctx = makeTestContext("diffdryrun");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nline2\nline3\n");

    string original = readTestFile(ctx, "test.txt");

    string diff = "--- a/test.txt\n+++ b/test.txt\n@@ -1,3 +1,3 @@\n line1\n-line2\n+line2modified\n line3\n";

    auto result = applyDiff(ctx, ApplyDiffParams("test.txt", diff, dryRun: true));
    assert(result.success, result.msg);

    // Verify file was NOT modified
    auto content = readTestFile(ctx, "test.txt");
    assert(content == original, content);

    // Verify preview contains modified content
    auto json = parseJSON(result.msg);
    assert(json["preview"].str.canFind("line2modified"), json.toString);
    assert(!json["preview"].str.canFind("-line2"), json.toString);
    assert(json["warnings"].array.empty, "correct counts should produce no warnings");
    assert(json["hunksApplied"].integer == 1, "expected 1 hunk applied");
    assert(json["linesChanged"].integer == 0, "expected 0 lines changed");
}

unittest {
    // Integration test: applyDiff with wrong hunk counts - applies with warnings
    auto ctx = makeTestContext("diffwarn");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nline2\nline3\nline4\n");

    // Hunk declares 3 old lines but body has 2; declares 2 new but body has 3
    string diff = "--- a/test.txt\n+++ b/test.txt\n@@ -1,3 +1,2 @@\n-line1\n-line2\n+line1a\n+line1b\n+line1c\n";

    auto result = applyDiff(ctx, ApplyDiffParams("test.txt", diff));
    assert(result.success, result.msg);

    auto json = parseJSON(result.msg);
    assert(json["hunksApplied"].integer == 1, json.toString);
    assert(json["warnings"].array.length == 2, json.toString);
    assert(json["warnings"][0].str.canFind("old lines"), json.toString);
    assert(json["warnings"][1].str.canFind("new lines"), json.toString);
    assert(json["linesChanged"].integer == 1, json.toString);

    // File content updated using body counts
    auto content = readTestFile(ctx, "test.txt");
    assert(content == "line1a\nline1b\nline1c\nline3\nline4\n", content);
}

unittest {
    // Integration test: applyDiffDryRun - error: empty diff
    auto ctx = makeTestContext("diffdryrun_empty");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\n");

    auto result = applyDiff(ctx, ApplyDiffParams("test.txt", "", dryRun: true));
    assert(!result.success);
    assert(result.msg.canFind("empty"), result.msg);
}

unittest {
    // Integration test: workarea confinement - absolute path rejected
    auto ctx = makeTestContext("workarea");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\n");

    // Absolute path should be rejected
    auto result = editFile(ctx, UnifiedEditFileParams(path: "/etc/passwd",
            content: "x", mode: "replace", marker: "marker"));
    assert(!result.success);
    assert(result.msg.canFind("absolute"), result.msg);
}

unittest {
    // Integration test: workarea confinement - path traversal rejected
    auto ctx = makeTestContext("workarea_traversal");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\n");

    // Path traversal should be rejected
    auto result = editFile(ctx, UnifiedEditFileParams(path: "../test.txt",
            content: "x", mode: "replace", marker: "marker"));
    assert(!result.success);
    assert(result.msg.canFind("workarea") || result.msg.canFind("outside"), result.msg);
}

unittest {
    // Integration test: editFile - invalid mode
    auto ctx = makeTestContext("invalidmode");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "x", mode: "invalid-mode", marker: "line1"));
    assert(!result.success);
    assert(result.msg.canFind("invalid"), result.msg);
}

unittest {
    // Integration test: editFile - whitespace-only search content
    auto ctx = makeTestContext("whitespaceonly");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "replacement", mode: "replace", searchContent: "   \n\n   "));
    assert(!result.success);
    assert(result.msg.canFind("non-empty"), result.msg);
}

unittest {
    // Integration test: editFile - remove mode with non-empty content
    auto ctx = makeTestContext("remove_content");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "not empty", mode: "remove", marker: "line1"));
    assert(!result.success);
    assert(result.msg.canFind("remove"), result.msg);
}

unittest {
    // Integration test: editFile - missing path (file does not exist)
    auto ctx = makeTestContext("missing_path");
    scope (exit)
        ctx.teardown;

    auto result = editFile(ctx, UnifiedEditFileParams(path: "nonexistent.txt",
            content: "x", mode: "replace", marker: "marker"));
    assert(!result.success);
}

unittest {
    // Integration test: editFile scope - byMarker found within scope
    auto ctx = makeTestContext("scope_marker_in");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "a\nmarker\nb\nmarker\nc\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt", content: "X",
            mode: "replace", marker: "marker", scopeStart: 2, scopeEnd: 3));
    assert(result.success, result.msg);

    auto json = parseJSON(result.msg);
    assert(json["matchedAt"].integer == 2, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "a\nX\nb\nmarker\nc\n", content);
}

unittest {
    // Integration test: editFile scope - marker outside scope errors with scope
    // mention, diagnostic includes the scope, file untouched
    auto ctx = makeTestContext("scope_marker_out");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "a\nmarker\nb\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt", content: "X",
            mode: "replace", marker: "marker", scopeStart: 3, scopeEnd: 3));
    assert(!result.success, result.msg);
    assert(result.msg.canFind("not found"), result.msg);
    assert(result.msg.canFind("within scope [3, 3]"), result.msg);

    auto json = parseJSON(result.msg);
    assert(json["diagnostic"]["scope"]["start"].integer == 3, result.msg);
    assert(json["diagnostic"]["scope"]["end"].integer == 3, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "a\nmarker\nb\n", content);
}

unittest {
    // Integration test: editFile scope - byContent with scopeStart/scopeEnd
    auto ctx = makeTestContext("scope_content");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "foo\nbar\nfoo\nbar\nend\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt", content: "X",
            mode: "replace", searchContent: "foo\nbar", scopeStart: 3, scopeEnd: 4));
    assert(result.success, result.msg);

    auto json = parseJSON(result.msg);
    assert(json["matchedAt"].integer == 3, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "foo\nbar\nX\nend\n", content);
}

unittest {
    // Integration test: editFile scope - byLine ignores scope
    auto ctx = makeTestContext("scope_byline_ignored");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "a\nb\nc\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt", content: "X",
            mode: "replace", startLine: 2, count: 1, scopeStart: 1, scopeEnd: 1));
    assert(result.success, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "a\nX\nc\n", content);
}

unittest {
    // Integration test: editFile scope - validation error for inverted range
    auto ctx = makeTestContext("scope_invalid");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "a\nb\nc\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "X", mode: "replace", marker: "b", scopeStart: 5, scopeEnd: 2));
    assert(!result.success);
    assert(result.msg.canFind("scopeStart"), result.msg);
}

unittest {
    // Integration test: editFile scope + dryRun - preview limited to scope,
    // file untouched
    auto ctx = makeTestContext("scope_dryrun");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "a\nmarker\nb\nmarker\nc\n");
    string original = readTestFile(ctx, "test.txt");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt", content: "X",
            mode: "replace", marker: "marker", scopeStart: 2, scopeEnd: 2, dryRun: true));
    assert(result.success, result.msg);

    auto json = parseJSON(result.msg);
    assert(json["matchedAt"].integer == 2, result.msg);
    assert(json["preview"].str == "a\nX\nb\nmarker\nc\n", result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == original, content); // dryRun never writes
}

// === Task 5: verifyContent match — edit proceeds normally ===
unittest {
    string[] fileLines = ["header", "line1", "line2", "line3", "footer"];
    auto outcome = executeByLine(fileLines, EditMode.replace, "new", 2, 2, false, "line1\nline2");
    assert(outcome.lines == ["header", "new", "line3", "footer"], outcome.lines.to!string);
    assert(outcome.linesChanged == -1); // 1 content line replaces 2 lines
}

// === Task 5: verifyContent mismatch — throws with actual content ===
unittest {
    string[] fileLines = ["header", "line1", "line2", "line3", "footer"];
    bool threw = false;
    try {
        executeByLine(fileLines, EditMode.replace, "new", 2, 2, false, "wrong content");
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("verifyContent mismatch"), e.msg);
        assert(e.msg.canFind("line1"), e.msg); // actual content in error
        assert(e.msg.canFind("wrong content"), e.msg); // expected in error
    }
    assert(threw);
}

// === Task 5: verifyContent with startLine past end of file ===
unittest {
    string[] fileLines = ["header"];
    bool threw = false;
    try {
        executeByLine(fileLines, EditMode.replace, "new", 100, 1, false, "target");
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("verifyContent mismatch"), e.msg);
        assert(e.msg.canFind("beyond end of file"), e.msg);
    }
    assert(threw);
}

// === Task 5: lineShift in success response via editFileUnifiedMemory ===
unittest {
    // Replace: contentLineCount=3, matchedLines=2 -> linesChanged=1
    auto outcome = editFileUnifiedMemory(["a", "b", "c", "d"],
            EditMode.replace, "X\nY\nZ", startLine: 2, count: 2);
    assert(outcome.linesChanged == 1);
    // Append: contentLineCount=2 -> linesChanged=2
    auto outcome2 = editFileUnifiedMemory(["a", "b", "c"], EditMode.append, "X\nY", startLine: 2);
    assert(outcome2.linesChanged == 2);
    // Remove: contentLineCount=0, matchedLines=1 -> linesChanged=-1
    auto outcome3 = editFileUnifiedMemory(["a", "b", "c"], EditMode.remove, "",
            startLine: 2, count: 1);
    assert(outcome3.linesChanged == -1);
    // Insert before: contentLineCount=2 -> linesChanged=2
    auto outcome4 = editFileUnifiedMemory(["a", "b", "c"],
            EditMode.insert_before, "X\nY", startLine: 2);
    assert(outcome4.linesChanged == 2);
}

// === Task 6: empty-line symmetric matching — file has blank line, search does not ===
unittest {
    // File has empty line between "foo" and "bar", search skips it
    string[] fileLines = ["foo", "", "bar"];
    string[] searchLines = ["foo", "bar"];
    auto result = findCodeBlock(fileLines, searchLines);
    assert(result.found);
    assert(result.start == 0);
    assert(result.end == 3); // range includes the empty line
}

// === Task 6: empty-line symmetric matching — search has blank line, file does not ===
unittest {
    // Search has empty line, file doesn't — empty search line is skipped
    string[] fileLines = ["foo", "bar"];
    string[] searchLines = ["foo", "", "bar"];
    auto result = findCodeBlock(fileLines, searchLines);
    assert(result.found);
    assert(result.start == 0);
    assert(result.end == 2);
}

// === Task 6: empty-line symmetric matching — both have blank lines ===
unittest {
    string[] fileLines = ["foo", "", "bar"];
    string[] searchLines = ["foo", "", "bar"];
    auto result = findCodeBlock(fileLines, searchLines);
    assert(result.found);
    assert(result.start == 0);
    assert(result.end == 3);
}

// === Task 6: empty-line symmetric matching — asymmetric blanks ===
unittest {
    // Two empty lines in file, one in search — all skipped
    string[] fileLines = ["foo", "", "", "bar"];
    string[] searchLines = ["foo", "", "bar"];
    auto result = findCodeBlock(fileLines, searchLines);
    assert(result.found);
    assert(result.start == 0);
    assert(result.end == 4); // range includes both empty lines
}

// === Task 6: empty-line symmetric matching — non-matching file line after skipped empties ===
unittest {
    // The second non-empty file line doesn't match search — should fail
    string[] fileLines = ["foo", "", "wrong", "bar"];
    string[] searchLines = ["foo", "bar"];
    auto result = findCodeBlock(fileLines, searchLines);
    assert(!result.found); // "wrong" != "bar" — no false match
}

// === Task 9: multi-line marker success — 3-line marker matches consecutive file lines ===
unittest {
    string[] fileLines = [
        "header", "void setTimer(int ms) {", "    // TODO: implement",
        "    timer = null;", "footer"
    ];
    auto outcome = editFileUnifiedMemory(fileLines, EditMode.replace,
            "void setTimer(int ms) {\n    timer = new Timer(ms);\n    timer.start();",
            marker: "void setTimer(int ms) {\n    // TODO: implement");
    // Auto-count from marker line count (2): replaces 2 lines with 3 → net +1 line
    assert(outcome.lines == [
        "header", "void setTimer(int ms) {", "    timer = new Timer(ms);",
        "    timer.start();", "    timer = null;", "footer"
    ], outcome.lines.to!string);
    assert(outcome.matched.matchedAt == 2);
    assert(outcome.matched.matchedLines == 2); // matched 2 marker lines
    assert(outcome.linesChanged == 1); // 3 content lines replace 2 matched lines
}

// === Task 9: multi-line marker partial fail — first line found, second line doesn't match ===
unittest {
    // First marker line "foo" matches at index 1; second marker line "wrong" does
    // NOT match "bar" → continues search. Eventually no full match is found.
    string[] fileLines = ["a", "foo", "bar", "foo", "baz", "end"];
    bool threw = false;
    try {
        editFileUnifiedMemory(fileLines, EditMode.replace, "X", marker: "foo\nwrong");
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("not found"), e.msg);
    }
    assert(threw);
}

// === Task 9: multi-line marker not found — first anchor line never found ===
unittest {
    string[] fileLines = ["a", "b", "c"];
    bool threw = false;
    try {
        editFileUnifiedMemory(fileLines, EditMode.replace, "X", marker: "zzz\nyyy");
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("not found"), e.msg);
    }
    assert(threw);
}

// === Task 9: multi-line marker with replaceAll — only full-anchor positions replaced ===
unittest {
    // Three "foo" lines, but only two have the matching second line "bar".
    // The middle "foo" has "baz" as the next line, so it is skipped.
    string[] fileLines = ["foo", "bar", "foo", "baz", "foo", "bar", "end"];
    auto outcome = editFileUnifiedMemory(fileLines, EditMode.replace, "X\nY",
            marker: "foo\nbar", replaceAll: true);
    assert(outcome.lines == ["X", "Y", "foo", "baz", "X", "Y", "end"], outcome.lines.to!string);
    assert(outcome.operations == 2);
}

// === Task 9: multi-line marker with scope — search limited to scope, anchor inside ===
unittest {
    // "foo\nbar" appears at lines 1-2 and 4-5. scopeStart=3, scopeEnd=5
    // restricts the anchor search to [2..5) (0-based), so only the second
    // occurrence (anchor at line 4, 0-based index 3) is targeted.
    string[] fileLines = ["foo", "bar", "header", "foo", "bar", "end"];
    auto outcome = editFileUnifiedMemory(fileLines, EditMode.replace, "X\nY",
            marker: "foo\nbar", scopeStart: 3, scopeEnd: 5);
    assert(outcome.lines == ["foo", "bar", "header", "X", "Y", "end"], outcome.lines.to!string);
    assert(outcome.matched.matchedAt == 4);
}

// === Task 9: multi-line marker > 20 lines — rejected ===
unittest {
    import std.algorithm : map;
    import std.array : join;
    import std.range : iota;

    // Build a 21-line marker (each line is unique)
    auto longMarker = iota(1, 22).map!(i => "line " ~ i.to!string).join("\n");
    bool threw = false;
    try {
        editFileUnifiedMemory(["a", "b"], EditMode.replace, "X", marker: longMarker);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("exceeds") || e.msg.canFind("20 lines")
                || e.msg.canFind("marker"), e.msg);
    }
    assert(threw);
}

// === Task 9: auto-count note — note describes marker-line auto-count ===
unittest {
    // Multi-line marker: count auto-derived from marker line count (2 lines)
    string[] fileLines = ["a", "step1", "    // details", "b"];
    auto outcome = editFileUnifiedMemory(fileLines, EditMode.replace,
            "new line", marker: "step1\n    // details");
    assert(outcome.autoCountUsed);
    assert(outcome.note.canFind("marker line count"), outcome.note);
    assert(outcome.matched.matchedLines == 2);
}

// === Task 9: auto-count note — note describes content-line auto-count ===
unittest {
    // Single-line marker with 2-line content: count auto-derived from content
    string[] fileLines = ["a", "marker", "old1", "old2", "b"];
    auto outcome = editFileUnifiedMemory(fileLines, EditMode.replace,
            "new1\nnew2", marker: "marker");
    assert(outcome.autoCountUsed);
    assert(outcome.note.canFind("content line count"), outcome.note);
    assert(outcome.matched.matchedLines == 2);
}

// === Task 9: byLine count missing — error message includes suggestion ===
unittest {
    bool threw = false;
    try {
        editFileUnifiedMemory(["a", "b"], EditMode.replace, "x", startLine: 1);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("count must be >= 1"), e.msg);
        assert(e.msg.canFind("Specify how many lines"), e.msg);
        assert(e.msg.canFind("byMarker"), e.msg);
        assert(e.msg.canFind("byContent"), e.msg);
    }
    assert(threw);
}

// === Task 9: multi-line marker with explicit count — autoCountUsed is false ===
unittest {
    string[] fileLines = ["a", "step1", "    // details", "extra", "b"];
    auto outcome = editFileUnifiedMemory(fileLines, EditMode.replace, "X\nY",
            marker: "step1\n    // details", count: 3);
    assert(!outcome.autoCountUsed);
    assert(outcome.note.empty);
    assert(outcome.matched.matchedLines == 3);
}
