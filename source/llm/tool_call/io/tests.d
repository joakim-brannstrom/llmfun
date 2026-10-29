module llm.tool_call.io.tests;

version (unittest) {
    import std.file : readText, mkdirRecurse;
    import std.stdio : File;
    import std.conv : to;
    import std.algorithm : canFind, startsWith;
    import std.array : empty, join;
    import std.json : parseJSON, JSONType;
    import std.string : splitLines;
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

        mixin FileDirtyTracking;
    }

    void createTestFile(TestFileContext ctx, string relativePath, string content) {
        assert(!relativePath.startsWith("/"), "relativePath must not be absolute: " ~ relativePath);
        auto fullPath = (ctx.workArea ~ relativePath).AbsolutePath;
        mkdirRecurse(fullPath.dirName.toString);
        auto f = File(fullPath.toString, "w");
        f.write(content);
    }

    string readTestFile(TestFileContext ctx, string relativePath) {
        auto fullPath = (ctx.workArea ~ relativePath).AbsolutePath;
        return readText(fullPath.toString);
    }

    TestFileContext makeTestContext(string testName) {
        import std.datetime : Clock;

        return new TestFileContext("./llmfun_test/" ~ testName ~ "_" ~ Clock.currTime.toString);
    }
}

@("parseEditMode parses mode names case-insensitively and throws on unknown modes")
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

@("findMarkerLine returns the first substring match index with start offset and endExclusive scope")
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

@(
        "findNthMarkerLine and countMarkerOccurrences find the nth occurrence and count with range limits")
unittest {
    string[] lines = ["a", "b", "a", "c", "a"];

    // findNthMarkerLine: Nth occurrence, 1-based
    assert(findNthMarkerLine(lines, "a", 1) == 0);
    assert(findNthMarkerLine(lines, "a", 2) == 2);
    assert(findNthMarkerLine(lines, "a", 3) == 4);
    assert(findNthMarkerLine(lines, "a", 4) == -1); // only 3 occurrences
    assert(findNthMarkerLine(lines, "zzz", 1) == -1); // not found at all

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

@("findCodeBlock finds the first trimmed-equality block with whitespace and CRLF tolerance and comment false-positive prevention")
unittest {
    // findCodeBlock: exact/whitespace/tab tolerance
    {
        string[] fileLines = ["foo", "bar", "baz"];
        string[] searchLines = ["foo", "bar"];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(result.found);
        assert(result.start == 0);
        assert(result.end == 2);
    }

    // Whitespace tolerance via strip
    {
        string[] fileLines = ["    foo", "  bar  ", "baz"];
        string[] searchLines = ["foo", "bar"];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(result.found);
        assert(result.start == 0);
        assert(result.end == 2);
    }

    // findCodeBlock: tabs/spaces tolerance & comment avoidance
    {
        string[] fileLines = ["\t\tfoo", "\tbar"];
        string[] searchLines = ["foo", "bar"];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(result.found);
        assert(result.start == 0);
        assert(result.end == 2);
    }

    // Comment false positive prevention: "// call fooBar()" should NOT match "foo"
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

    // findCodeBlock: empty/whitespace/multi-line/anchor checks
    {
        auto result = findCodeBlock(["foo"], cast(string[])[]);
        assert(!result.found);
    }

    // All-empty search block returns not found
    {
        auto result = findCodeBlock(["foo"], ["", "  ", "\t"]);
        assert(!result.found);
    }

    // Leading empty lines in search content
    {
        string[] fileLines = ["foo", "bar"];
        string[] searchLines = ["", "", "foo", "bar"];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(result.found);
        assert(result.start == 0);
        assert(result.end == 2);
    }

    // Multi-line search block matching
    {
        string[] fileLines = ["function foo() {", "    return bar;", "}"];
        string[] searchLines = ["function foo() {", "    return bar;", "}"];
        auto result = findCodeBlock(fileLines, searchLines);
        assert(result.found);
        assert(result.start == 0);
        assert(result.end == 3);
    }

    // Empty search lines in middle (flexible)
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

@(
        "findNthCodeBlock finds the nth non-overlapping block and countCodeBlockOccurrences counts by anchor within range")
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

@(
        "editFileInMemory applies each mode in memory and reports the net linesChanged without mutating the input")
unittest {
    // === replace: 3-line block replaced with 2-line content -> linesChanged = -1 ===
    {
        string[] fileLines = ["a", "b", "c", "d", "e", "f"];
        auto res = editFileInMemory(fileLines, EditMode.replace, "x\ny", EditTarget(2, 5, 3, 3));
        assert(res.lines == ["a", "b", "x", "y", "f"], res.lines.to!string);
        assert(res.linesChanged == -1, res.linesChanged.to!string);
    }

    // remove: verifies deletion and linesChanged update
    {
        string[] fileLines = ["a", "b", "c", "d", "e", "f", "g"];
        auto res = editFileInMemory(fileLines, EditMode.remove, "", EditTarget(1, 6, 2, 5));
        assert(res.lines == ["a", "g"], res.lines.to!string);
        assert(res.linesChanged == -5, res.linesChanged.to!string);
    }

    // append: verifies content insertion and linesChanged update
    {
        string[] fileLines = ["a", "b", "c"];
        auto res = editFileInMemory(fileLines, EditMode.append, "x\ny", EditTarget(1, 1, 2, 1));
        assert(res.lines == ["a", "b", "x", "y", "c"], res.lines.to!string);
        assert(res.linesChanged == 2, res.linesChanged.to!string);
    }

    // insert_after: alias for append
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

@("edit dispatch echoes the exact consumed pre-edit target lines in replacedLines")
unittest {
    // === full echo: byLine replace consumes exactly the 3 old lines ===
    {
        string[] fileLines = ["a", "b", "c", "d", "e", "f"];
        auto outcome = executeByLine(fileLines, EditMode.replace, "x\ny", 3, 3, false);
        assert(outcome.replacedLines == ["c", "d", "e"], outcome.replacedLines.to!string);
        assert(outcome.replacedTotal == 3, outcome.replacedTotal.to!string);
    }

    // === byMarker single occurrence with count=2 consumes marker + next line ===
    {
        string[] fileLines = ["head", "m", "old1", "tail"];
        auto outcome = executeByMarker(fileLines, EditMode.replace, "X", ["X"],
                1, "m", 2, false, 1, 0, fileLines.length, -1, -1);
        assert(outcome.replacedLines == ["m", "old1"], outcome.replacedLines.to!string);
        assert(outcome.replacedTotal == 2, outcome.replacedTotal.to!string);
    }

    // === byContent single occurrence consumes the full matched block ===
    {
        string[] fileLines = ["head", "b", "c", "tail"];
        auto outcome = executeByContent(fileLines, EditMode.replace, "X",
                ["X"], "b\nc", 0, false, 1, 0, fileLines.length, -1, -1);
        assert(outcome.replacedLines == ["b", "c"], outcome.replacedLines.to!string);
        assert(outcome.replacedTotal == 2, outcome.replacedTotal.to!string);
    }

    // === replaceAll over 2 byMarker occurrences accumulates both blocks in order ===
    {
        string[] fileLines = ["m", "old1", "m", "old2", "tail"];
        auto outcome = executeByMarker(fileLines, EditMode.replace, "X", ["X"],
                1, "m", 2, true, 1, 0, fileLines.length, -1, -1);
        assert(outcome.replacedLines == ["m", "old1", "m", "old2"],
                outcome.replacedLines.to!string);
        assert(outcome.replacedTotal == 4, outcome.replacedTotal.to!string);
    }

    // === replaceAll over 2 byContent blocks accumulates both blocks in order ===
    {
        string[] fileLines = ["head", "b", "c", "mid", "b", "c", "tail"];
        auto outcome = executeByContent(fileLines, EditMode.replace, "X",
                ["X"], "b\nc", 0, true, 1, 0, fileLines.length, -1, -1);
        assert(outcome.replacedLines == ["b", "c", "b", "c"], outcome.replacedLines.to!string);
        assert(outcome.replacedTotal == 4, outcome.replacedTotal.to!string);
    }

    // === insert modes consume nothing ===
    {
        string[] fileLines = ["a", "b", "c"];
        auto ins = executeByLine(fileLines, EditMode.insert_after, "x", 2, 0, false);
        assert(ins.replacedLines.empty, ins.replacedLines.to!string);
        assert(ins.replacedTotal == 0, ins.replacedTotal.to!string);
    }

    // === no-targeting append-at-EOF shape consumes nothing ===
    {
        string[] fileLines = ["a", "b"];
        auto outcome = editFileUnifiedMemory(fileLines, EditMode.append, "x");
        assert(outcome.replacedLines.empty, outcome.replacedLines.to!string);
        assert(outcome.replacedTotal == 0, outcome.replacedTotal.to!string);
    }

    // === byLine off-by-one over-consumption is visible in replacedLines ===
    // (the struct-closing brace was eaten)
    {
        string[] fileLines = ["a", "b", "c", "d", "}"];
        auto outcome = executeByLine(fileLines, EditMode.replace, "REPL", 4, 2, false);
        assert(outcome.replacedLines == ["d", "}"], outcome.replacedLines.to!string);
        assert(outcome.replacedTotal == 2, outcome.replacedTotal.to!string);
    }

    // === dispatch carries the FULL lines; elision is a tool render concern ===
    {
        string[] fileLines = [
            "l1", "l2", "l3", "l4", "l5", "l6", "l7", "l8", "l9", "l10",
            "l11", "l12", "l13", "l14"
        ];
        auto outcome = executeByLine(fileLines, EditMode.replace, "x", 2, 12, false);
        assert(outcome.replacedLines.length == 12, outcome.replacedLines.length.to!string);
        assert(outcome.replacedTotal == 12, outcome.replacedTotal.to!string);
    }
}

@(
        "log helpers sanitize paths, classify edit error strings, extract hunk numbers, and format scope text")
unittest {
    // Structured logging helpers.
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
    assert(classifyEditError("no targeting method specified. Provide exactly one of: startLine (byLine), marker (byMarker), or searchContent (byContent), or use mode=append with no targeting to append at the end of the file") == "missing_targeting");
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

    assert(classifyEditError("marker 'm' matched as a strict substring inside longer line(s); the edit was refused before any change. Matched lines longer than the marker: line 2: 'target'. Pass a longer marker that covers the full line(s), or pass fragmentOk=true to replace whole lines deliberately.") == "fragment_guard");
    assert(classifyEditError("marker matches 1 line but content has 3 lines; pass count=3 to replace 3 lines starting at the marker, or use a multi-line marker to auto-size the target") == "auto_count_required");

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
    assert(targetingMethodOf(UnifiedEditFileParams("p.txt", "x", "replace")) == "none");
    assert(targetingMethodOf(UnifiedEditFileParams("p.txt", "x", "append")) == "append_eof");
    assert(targetingMethodOf(UnifiedEditFileParams("p.txt", "x", "insert_after")) == "none");
    assert(targetingMethodOf(UnifiedEditFileParams("p.txt", "x", "replace",
            startLine: 1, marker: "m")) == "none");

    assert(targetingMethodOf(UnifiedEditFileParams("p.txt", "x", "replace",
            marker: "m", scopeStart: 5, scopeEnd: 10)) == "byMarker");

    // Scope helpers.
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

@("editFile @ParamDescription texts state the actual line-granular semantics")
unittest {
    import std.algorithm : canFind;
    import llm.tool_call : ParamDescription;

    string descOf(string member)() {
        foreach (a; __traits(getAttributes, __traits(getMember, UnifiedEditFileParams, member)))
            static if (is(typeof(a) == ParamDescription))
                return a.value;
        assert(false, "no @ParamDescription on " ~ member);
    }

    assert(descOf!"marker".canFind("line-granular"));
    assert(descOf!"searchContent".canFind("trimmed equality"));
    assert(descOf!"mode".canFind("KEPT"));
    assert(descOf!"count".canFind("pass count=") && descOf!"count".canFind("multi-line marker"));
    assert(descOf!"replaceAll".canFind("line-granular"));
}

@(
        "integration: editFile byMarker replace with a multi-line marker behaves identically (texts changed, semantics not)")
unittest {
    auto ctx = makeTestContext("edit_marker_multiline_nf1");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "begin\nend\nafter\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "X\nY", mode: "replace", marker: "begin\nend"));
    assert(result.success, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "X\nY\nafter\n", content);
}

@("byLine replace replaces the correct lines with correct match metadata")
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

@("byLine replace with identical content is an exact no-op (noOp + note)")
unittest {
    // === byLine + replace where the new content equals the replaced text: the
    // composed result is element-wise identical to the input, so it is an
    // exact no-op. Detection is sequence equality on the composed lines,
    // never the linesChanged net delta (which is 0 for real changes too:
    // replacing N lines with N different lines). ===
    string[] fileLines = ["a", "b", "c"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "b", startLine: 2, count: 1);
    assert(res.lines == ["a", "b", "c"], res.lines.to!string);
    assert(res.linesChanged == 0, res.linesChanged.to!string);
    assert(res.noOp, "identical-content replace must set noOp");
}

@("replace with equal line count but DIFFERENT lines has linesChanged == 0 without noOp")
unittest {
    // === counterexample: the net line-count delta is neither necessary nor
    // sufficient for a no-op. ===
    string[] fileLines = ["a", "b", "c"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "x\ny", startLine: 1, count: 2);
    assert(res.lines == ["x", "y", "c"], res.lines.to!string);
    assert(res.linesChanged == 0, res.linesChanged.to!string);
    assert(!res.noOp, "a real change must never be flagged as a no-op");
}

@("remove of consumed lines is not flagged as a no-op")
unittest {
    // A remove that consumes lines is never element-wise equal to the input.
    string[] fileLines = ["a", "b", "c", "d"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.remove, "", startLine: 2, count: 2);
    assert(res.lines == ["a", "d"], res.lines.to!string);
    assert(res.linesChanged == -2, res.linesChanged.to!string);
    assert(!res.noOp);
}

@("insert modes never set noOp, even when nothing is inserted (empty content)")
unittest {
    // Insert with empty content consumes nothing (no old-vs-new text pair to
    // compare), so it is excluded from no-op detection: nothing is consumed
    // and the no-op is self-evident from linesChanged == 0. A flag there
    // would be a special case with no audit value.
    string[] fileLines = ["a", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.append, "");
    assert(res.lines == ["a", "b"], res.lines.to!string);
    assert(res.linesChanged == 0, res.linesChanged.to!string);
    assert(!res.noOp, "insert with empty content is excluded from noOp");

    auto res2 = editFileUnifiedMemory(fileLines, EditMode.insert_before, "", startLine: 1);
    assert(res2.lines == ["a", "b"], res2.lines.to!string);
    assert(!res2.noOp, "insert with empty content is excluded from noOp");

    auto res3 = editFileUnifiedMemory(fileLines, EditMode.insert_after, "X", startLine: 1);
    assert(res3.lines == ["a", "X", "b"], res3.lines.to!string);
    assert(!res3.noOp, "insert modes never set noOp");
}

@("insert_after content's first line duplicating the kept anchor warns (anchorDuplicate)")
unittest {
    // === byMarker insert_after keeps the marker line as the anchor; when the
    // content's FIRST line equals it, the anchor text appears twice. Warn
    // only: the edit is never mutated (no magic stripping). ===
    string[] fileLines = ["m", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.insert_after, " m\nX", marker: "m");
    // Trimmed equality on both sides: content " m" matches anchor "m".
    assert(res.anchorDuplicate, "boundary duplication must warn");
    // The composed lines keep the raw content lines (only the comparison
    // trims): content " m" stays " m" in the result.
    // The composed lines keep the raw content lines (only the comparison
    // trims): content " m" stays " m" in the result.
    assert(res.lines == ["m", " m", "X", "b"], res.lines.to!string);
    assert(!res.noOp, "insert modes never set noOp");
}

@("insert_before content's last line duplicating the kept anchor warns (anchorDuplicate)")
unittest {
    // === byMarker insert_before mirror: the content's LAST line is compared
    // against the kept anchor line. ===
    string[] fileLines = ["a", "m"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.insert_before, "X\nm", marker: "m");
    assert(res.anchorDuplicate, "boundary duplication must warn");
    assert(res.lines == ["a", "X", "m", "m"], res.lines.to!string);
    assert(!res.noOp, "insert modes never set noOp");
}

@("byContent insert_after content's first line duplicating the block's last line warns")
unittest {
    // === byContent insert_after anchors on the matched block's LAST line; a
    // duplicated content first line warns the same way as byMarker. ===
    string[] fileLines = ["head", "foo", "bar", "tail"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.insert_after,
            "bar\nbaz", searchContent: "foo\nbar");
    assert(res.anchorDuplicate, "boundary duplication must warn");
    assert(res.lines == ["head", "foo", "bar", "bar", "baz", "tail"], res.lines.to!string);
}

@("byContent insert_before content's last line duplicating the block's first line warns")
unittest {
    // === byContent insert_before mirror: anchors on the block's FIRST line. ===
    string[] fileLines = ["head", "foo", "bar", "tail"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.insert_before,
            "baz\nfoo", searchContent: "foo\nbar");
    assert(res.anchorDuplicate, "boundary duplication must warn");
    assert(res.lines == ["head", "baz", "foo", "foo", "bar", "tail"], res.lines.to!string);
}

@("non-duplicating inserts stay silent (no anchorDuplicate flag or note)")
unittest {
    // === Only the content's boundary line is compared against the anchor:
    // a later interior duplicate does NOT warn, and ordinary inserts are
    // never flagged. ===
    string[] fileLines = ["m", "b"];

    // Interior duplication: content's SECOND line equals the anchor — the
    // first line does not, so no warning.
    auto res = editFileUnifiedMemory(fileLines, EditMode.insert_after, "X\nm", marker: "m");
    assert(!res.anchorDuplicate, "only the boundary line is compared");
    assert(res.lines == ["m", "X", "m", "b"], res.lines.to!string);

    // Ordinary insert, nothing in common with the anchor.
    auto res2 = editFileUnifiedMemory(fileLines, EditMode.insert_after, "X", marker: "m");
    assert(!res2.anchorDuplicate, "ordinary inserts must stay silent");
    assert(res2.lines == ["m", "X", "b"], res2.lines.to!string);

    // byContent insert with no boundary duplication.
    string[] fileLines2 = ["head", "foo", "bar"];
    auto res3 = editFileUnifiedMemory(fileLines2, EditMode.insert_after,
            "new\nlines", searchContent: "foo\nbar");
    assert(!res3.anchorDuplicate, "ordinary inserts must stay silent");
    assert(res3.lines == ["head", "foo", "bar", "new", "lines"], res3.lines.to!string);
}

@("targeted append (insert_after's alias) warns on anchor duplication like insert_after")
unittest {
    // === append shares the engine's insert_after case, so a marker-anchored
    // append with a duplicating content boundary line warns the same way. ===
    string[] fileLines = ["m", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.append, "m\nX", marker: "m");
    assert(res.anchorDuplicate, "append is insert_after's alias and must warn");
    assert(res.lines == ["m", "m", "X", "b"], res.lines.to!string);
    assert(!res.noOp, "insert modes never set noOp");
}

@("byMarker replaceAll with identical content at every occurrence is an exact no-op")
unittest {
    // The replaceAll loop's composed result must be compared against the
    // ORIGINAL lines (the appender output), not just per occurrence.
    string[] fileLines = ["m", "keep", "m"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "m", marker: "m",
            replaceAll: true);
    assert(res.lines == ["m", "keep", "m"], res.lines.to!string);
    assert(res.linesChanged == 0, res.linesChanged.to!string);
    assert(res.operations == 2, res.operations.to!string);
    assert(res.noOp, "replaceAll composing an identical file must set noOp");
}

@("byContent replace with count omitted and identical content: noOp coexists with autoCount")
unittest {
    // autoCount (count auto-derived from the matched block size) and noOp CAN
    // coexist: a byContent replace with count omitted whose content equals the
    // matched block. Both flags are set; the shared `note` slot is combined at
    // the tool layer (engine-level note stays the autoCount derivation note).
    string[] fileLines = ["head", "foo", "bar", "tail"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "foo\nbar",
            searchContent: "foo\nbar");
    assert(res.lines == ["head", "foo", "bar", "tail"], res.lines.to!string);
    assert(res.linesChanged == 0, res.linesChanged.to!string);
    assert(res.autoCountUsed, "count auto-derived from the matched block size");
    assert(res.noOp, "identical-content replace must set noOp");
}

@("byMarker replace with multi-line content and explicit count replaces that many lines")
unittest {
    // === byMarker + replace + multi-line content + explicit count: the
    // content-line-count auto-derive was removed (over-consumption hole), so
    // multi-line-content shapes now require an explicit count. ===
    string[] fileLines = ["a", "marker", "b", "c", "d"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "X\nY\nZ",
            marker: "marker", count: 3);
    assert(res.lines == ["a", "X", "Y", "Z", "d"], res.lines.to!string);
    assert(res.matched.matchedAt == 2);
    assert(res.matched.matchedLines == 3, res.matched.matchedLines.to!string);
    assert(!res.autoCountUsed);
    assert(res.note.empty);
    assert(res.linesChanged == 0, res.linesChanged.to!string);
}

@("byMarker replace with single-line content and no count defaults to count=1")
unittest {
    // === byMarker + replace + single-line content + no count → count=1 ===
    string[] fileLines = ["a", "marker", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "X", marker: "marker");
    assert(res.lines == ["a", "X", "b"], res.lines.to!string);
    assert(res.matched.matchedLines == 1);
    assert(!res.autoCountUsed);
}

@("byMarker replace with multi-line content and explicit count=1 replaces only the anchor line")
unittest {
    // === byMarker + replace + multi-line content + explicit count=1 ===
    string[] fileLines = ["a", "marker", "b", "c"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "X\nY",
            marker: "marker", count: 1);
    assert(res.lines == ["a", "X", "Y", "b", "c"], res.lines.to!string);
    assert(res.matched.matchedLines == 1);
    assert(!res.autoCountUsed);
}

@("byMarker replace with explicit count larger than the content replaces that many lines")
unittest {
    // === byMarker + replace + explicit count > content lines ===
    string[] fileLines = ["a", "marker", "b", "c", "d"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "X", marker: "marker", count: 3);
    assert(res.lines == ["a", "X", "d"], res.lines.to!string);
    assert(res.linesChanged == -2, res.linesChanged.to!string);
    assert(res.matched.matchedLines == 3);
}

@("byMarker replace with multi-line content and no count errors with auto-count guidance")
unittest {
    // === The over-consumption shape (single-line marker + multi-line
    // content + count<1) used to auto-derive count from the CONTENT line
    // count; it now hard-errors with guidance before any EOF check. ===
    bool threw = false;
    try {
        editFileUnifiedMemory(["a", "marker", "b"], EditMode.replace, "X\nY\nZ", marker: "marker");
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("marker matches 1 line but content has 3 lines"), e.msg);
        assert(e.msg.canFind("pass count=3"), e.msg);
    }
    assert(threw, "over-consumption shape must hard-error");
}

@("byContent replace finds the block by trimmed equality")
unittest {
    // === byContent + replace: block found by trimmed equality ===
    string[] fileLines = ["a", "foo() {", "  bar();", "}", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "new()",
            searchContent: "foo() {\n  bar();\n}");
    assert(res.lines == ["a", "new()", "b"], res.lines.to!string);
    assert(res.matched.matchedAt == 2);
    assert(res.matched.matchedLines == 3, res.matched.matchedLines.to!string);
}

@("byContent replaceAll replaces every non-overlapping occurrence")
unittest {
    // === byContent + replaceAll: all occurrences replaced, non-overlapping ===
    string[] fileLines = ["x", "foo", "y", "foo", "z"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "bar",
            searchContent: "foo", replaceAll: true);
    assert(res.lines == ["x", "bar", "y", "bar", "z"], res.lines.to!string);
    assert(res.operations == 2, res.operations.to!string);
    assert(res.matched.matchedAt == 2);
}

@("byContent replaceAll never re-matches replacement text that contains the pattern")
unittest {
    // === byContent + replaceAll is non-overlapping: replacement content that
    // contains the search pattern is not re-matched ===
    string[] fileLines = ["a", "b", "c"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "b\nx",
            searchContent: "b", replaceAll: true);
    assert(res.lines == ["a", "b", "x", "c"], res.lines.to!string);
    assert(res.operations == 1, res.operations.to!string);
}

@("byMarker replaceAll replaces each marker line")
unittest {
    // === byMarker + replaceAll: each marker line replaced ===
    string[] fileLines = ["a", "marker", "b", "marker", "c"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "X", marker: "marker",
            replaceAll: true);
    assert(res.lines == ["a", "X", "b", "X", "c"], res.lines.to!string);
    assert(res.operations == 2);
    assert(res.matched.matchedAt == 2);
}

@("byMarker replace with a single-line fragment marker is refused without fragmentOk and reports the replaced lines with it")
unittest {
    // === byMarker + replace: the marker matched as a strict substring inside
    // a longer line, so a whole-line replace would destroy the rest of that
    // line (13-line config.d incident). Refused before any change by default;
    // with fragmentOk=true the WHOLE line is replaced and the count is echoed. ===
    string[] fileLines = ["int config = compute(a, b, c, d);", "end"];
    bool threw = false;
    try {
        editFileUnifiedMemory(fileLines, EditMode.replace,
                "int config = compute(x, y);", marker: "compute(a, b, c, d)");
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("matched as a strict substring inside longer line"), e.msg);
        assert(e.msg.canFind("line 1: 'int config = compute(a, b, c, d);'"), e.msg);
        assert(e.msg.canFind("fragmentOk=true"), e.msg);
    }
    assert(threw, "fragment marker replace must be refused by default");

    auto res = editFileUnifiedMemory(fileLines, EditMode.replace,
            "int config = compute(x, y);", marker: "compute(a, b, c, d)", fragmentOk: true);
    assert(res.lines == ["int config = compute(x, y);", "end"], res.lines.to!string);
    assert(res.fragmentLinesReplaced == 1, res.fragmentLinesReplaced.to!string);
}

@("byMarker replace with a multi-line marker whose verified trailing line strictly contains its marker line is refused")
unittest {
    // === multi-line marker: both lines are verified as substrings; the
    // trailing file line is strictly longer than its marker line, so the text
    // outside the trailing match is destroyed by the replace. Exact equality
    // (anchor and trailing line) is NOT a fragment: the edit proceeds. ===
    string[] fileLines = ["begin foo bar", "end baz qux", "after"];
    bool threw = false;
    try {
        editFileUnifiedMemory(fileLines, EditMode.replace, "X\nY", marker: "foo\nend");
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("matched as a strict substring inside longer line"), e.msg);
        assert(e.msg.canFind("line 1: 'begin foo bar'"), e.msg);
        assert(e.msg.canFind("line 2: 'end baz qux'"), e.msg); // both offending lines listed
    }
    assert(threw, "multi-line fragment marker replace must be refused");

    auto res = editFileUnifiedMemory(["foo", "end", "after"], EditMode.replace,
            "X\nY", marker: "foo\nend");
    assert(res.lines == ["X", "Y", "after"], res.lines.to!string);
    assert(res.fragmentLinesReplaced == 0, res.fragmentLinesReplaced.to!string);
}

@("fragmentOk with byContent, byLine, or non-replace modes errors as invalid")
unittest {
    // === Validity mirror of resolveTargeting: fragmentOk acknowledges a
    // byMarker replace-only hazard, so it must be rejected with the other
    // targeting methods and with the modes it has no effect in. ===
    bool threw = false;
    try {
        editFileUnifiedMemory(["a", "b"], EditMode.replace, "x", searchContent: "b",
                fragmentOk: true);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("fragmentOk"), e.msg);
        assert(e.msg.canFind("byContent"), e.msg);
    }
    assert(threw, "fragmentOk with byContent must error");

    threw = false;
    try {
        editFileUnifiedMemory(["a", "b"], EditMode.replace, "x", startLine: 1, fragmentOk: true);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("fragmentOk"), e.msg);
        assert(e.msg.canFind("byLine"), e.msg);
    }
    assert(threw, "fragmentOk with byLine must error");

    threw = false;
    try {
        editFileUnifiedMemory(["a", "b"], EditMode.append, "x", marker: "b", fragmentOk: true);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("fragmentOk"), e.msg);
        assert(e.msg.canFind("replace mode"), e.msg);
    }
}

@("fragmentOk with append and no targeting is rejected instead of silently appending")
unittest {
    bool threw = false;
    try {
        editFileUnifiedMemory(["a", "b"], EditMode.append, "x", fragmentOk: true);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("fragmentOk is only valid with replace mode"), e.msg);
    }
    assert(threw, "fragmentOk with append and no targeting must error");
}

@(
        "byLine + replaceAll + fragmentOk surfaces the replaceAll contradiction, not the fragmentOk error")
unittest {
    bool threw = false;
    try {
        editFileUnifiedMemory(["a", "b"], EditMode.replace, "x", startLine: 1,
                count: 1, replaceAll: true, fragmentOk: true);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("replaceAll is not supported with byLine"), e.msg);
    }
    assert(threw, "byLine + replaceAll + fragmentOk must surface the replaceAll error");
}

@("fragmentOk with replace and no targeting keeps the no-targeting error")
unittest {
    bool threw = false;
    try {
        editFileUnifiedMemory(["a", "b"], EditMode.replace, "x", fragmentOk: true);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("no targeting method specified"), e.msg);
    }
    assert(threw, "fragmentOk with replace and no targeting must keep the no-targeting error");
}

@("byMarker matchIndex=2 refused when the targeted occurrence is a fragment")
unittest {
    // === Single-occurrence path with matchIndex > 1: occurrence 1 is an exact
    // match, occurrence 2 is a fragment - the guard refuses and reports the
    // occurrence-2 line (the replaceAll variant is covered above). ===
    string[] fileLines = ["foo", "pre foo post"];
    bool threw = false;
    try {
        editFileUnifiedMemory(fileLines, EditMode.replace, "X", marker: "foo", matchIndex: 2);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("matched as a strict substring inside longer line"), e.msg);
        assert(e.msg.canFind("line 2: 'pre foo post'"), e.msg);
    }
    assert(threw, "matchIndex=2 onto a fragment occurrence must be refused");
}

@("byMarker replaceAll sums fragmentLinesReplaced and refuses on the first offending occurrence")
unittest {
    // === replaceAll with fragmentOk=true: the guard is checked per occurrence
    // and the per-match counts are SUMMED into the echo. The guard fires on the
    // FIRST offending match: occurrence 1 is an exact match, occurrence 2 is a
    // fragment - the whole edit is still refused. ===
    string[] fileLines = ["pre foo post", "mid", "pre foo again post"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "X", marker: "foo",
            replaceAll: true, fragmentOk: true);
    assert(res.lines == ["X", "mid", "X"], res.lines.to!string);
    assert(res.operations == 2, res.operations.to!string);
    assert(res.fragmentLinesReplaced == 2, res.fragmentLinesReplaced.to!string);

    string[] mixed = ["foo", "pre foo post"];
    bool threw = false;
    try {
        editFileUnifiedMemory(mixed, EditMode.replace, "X", marker: "foo", replaceAll: true);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("line 2: 'pre foo post'"), e.msg);
    }
    assert(threw, "replaceAll must refuse on the first offending occurrence");
}

@("integration: the config.d-style fragment incident is refused without fragmentOk and the whole line is replaced with it")
unittest {
    // === 13-line config.d incident end-to-end: marker "computeValue" matched
    // as a strict substring inside the long signature line; count=13 would
    // replace the whole file from that line down. Refused without fragmentOk
    // (nothing written); with it the WHOLE line is replaced and the remainder
    // is intact. dryRun previews get the same refusal. ===
    auto ctx = makeTestContext("fragment_incident");
    scope (exit)
        ctx.teardown;

    string[] configLines = [
        "module my.config;", "import std.stdio;",
        "int computeValue(int alpha, int beta, int gamma, int delta) {",
        "    return alpha + beta + gamma + delta;", "}", "", "void main() {",
        "    import std.conv : to;", "    const total = computeValue(1, 2, 3, 4);",
        "    writeln(total.to!string);", "}", "", "int helper(int a, int b) {",
        "    return a * b;", "}", "// end of file",
    ];
    createTestFile(ctx, "config.d", configLines.join("\n") ~ "\n");

    auto refused = editFile(ctx, UnifiedEditFileParams(path: "config.d", content: "int computeValue(int x, int y) {",
            mode: "replace", marker: "computeValue", count: 13));
    assert(!refused.success, refused.msg);
    assert(refused.msg.canFind("matched as a strict substring inside longer line"), refused.msg);
    assert(refused.msg.canFind("line 3: 'int computeValue(int alpha"), refused.msg);
    assert(refused.msg.canFind("fragmentOk=true"), refused.msg);
    assert(readTestFile(ctx, "config.d") == configLines.join("\n") ~ "\n",
            "refused edit must not write");

    auto dryRun = editFile(ctx, UnifiedEditFileParams(path: "config.d", content: "int computeValue(int x, int y) {",
            mode: "replace", marker: "computeValue", count: 13, dryRun: true));
    assert(!dryRun.success, dryRun.msg);
    assert(dryRun.msg.canFind("matched as a strict substring inside longer line"), dryRun.msg);
    assert(readTestFile(ctx, "config.d") == configLines.join("\n") ~ "\n",
            "dryRun refusal must not write");

    auto ok = editFile(ctx, UnifiedEditFileParams(path: "config.d", content: "int computeValue(int x, int y) {",
            mode: "replace", marker: "computeValue", count: 13, fragmentOk: true));
    assert(ok.success, ok.msg);
    auto json = parseJSON(ok.msg);
    assert(json["fragmentLinesReplaced"].integer == 1, ok.msg);
    assert(readTestFile(ctx,
            "config.d") == "module my.config;\nimport std.stdio;\nint computeValue(int x, int y) {\n// end of file\n",
            readTestFile(ctx, "config.d"));
}

@("append with byMarker keeps the marker line and inserts after it")
unittest {
    // === append with byMarker keeps the marker line ===
    string[] fileLines = ["a", "marker", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.append, "X", marker: "marker");
    assert(res.lines == ["a", "marker", "X", "b"], res.lines.to!string);
    assert(res.matched.matchedAt == 2);
}

@("insert_before with byContent inserts before the matched block")
unittest {
    // === insert_before with byContent inserts before the matched block ===
    string[] fileLines = ["a", "foo", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.insert_before, "X", searchContent: "foo");
    assert(res.lines == ["a", "X", "foo", "b"], res.lines.to!string);
}

@("remove with byContent deletes the matched block")
unittest {
    // === remove with byContent deletes the matched block ===
    string[] fileLines = ["a", "foo", "bar", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.remove, "", searchContent: "foo\nbar");
    assert(res.lines == ["a", "b"], res.lines.to!string);
    assert(res.linesChanged == -2);
}

@("byContent with explicit count extends the replaced range")
unittest {
    string[] fileLines = ["a", "foo", "bar", "baz", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "X",
            searchContent: "foo", count: 3);
    assert(res.lines == ["a", "X", "b"], res.lines.to!string);
    assert(res.matched.matchedLines == 3);
}

@("empty content in replace mode deletes the targeted line")
unittest {
    string[] fileLines = ["a", "marker", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "", marker: "marker");
    assert(res.lines == ["a", "b"], res.lines.to!string);
    assert(res.linesChanged == -1);
}

@("ambiguous targeting with multiple methods errors naming both")
unittest {
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

@("missing targeting method errors out")
unittest {
    bool threw = false;
    try {
        editFileUnifiedMemory(["a"], EditMode.replace, "x");
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("no targeting method"), e.msg);
    }
    assert(threw);
}

@("append with no targeting appends at the end with matchedAt one past the last line")
unittest {
    // === append with NO targeting: appends at the end of the file ===
    // Zero targeting + mode=append is a pure insertion one past the last
    // line. matchedAt is the 1-based line where the new content starts
    // (fileLines.length + 1) - deliberately DIVERGENT from byLine, which at
    // EOF (startLine == file length) reports the ANCHOR line (length); the
    // difference is documented in editFile's @Function description.
    string[] fileLines = ["a", "b", "c"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.append, "X");
    assert(res.lines == ["a", "b", "c", "X"], res.lines.to!string);
    assert(res.matched.matchedAt == 4, res.matched.to!string);
    assert(res.matched.matchedLines == 1, res.matched.to!string);
    assert(res.linesChanged == 1, res.linesChanged.to!string);
    assert(!res.autoCountUsed);
}

@("byLine append at EOF reports the anchor line as matchedAt")
unittest {
    string[] fileLines = ["a", "b", "c"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.append, "X", startLine: 3);
    assert(res.lines == ["a", "b", "c", "X"], res.lines.to!string);
    assert(res.matched.matchedAt == 3, res.matched.to!string);
}

@("append with no targeting on an empty file inserts at line 1")
unittest {
    auto res = editFileUnifiedMemory([], EditMode.append, "X");
    assert(res.lines == ["X"], res.lines.to!string);
    assert(res.matched.matchedAt == 1, res.matched.to!string);
    assert(res.linesChanged == 1, res.linesChanged.to!string);
}

@("no targeting with non-append modes fails with the use mode=append hint")
unittest {
    foreach (mode; [
        EditMode.replace, EditMode.insert_after, EditMode.insert_before,
        EditMode.remove
    ]) {
        bool threw = false;
        try {
            editFileUnifiedMemory(["a"], mode, mode == EditMode.remove ? "" : "x");
        } catch (Exception e) {
            threw = true;
            assert(e.msg.canFind("no targeting method"), e.msg);
            assert(e.msg.canFind("mode=append with no targeting"), e.msg);
        }
        assert(threw);
    }
}

@("byMarker not found errors out")
unittest {
    bool threw = false;
    try {
        editFileUnifiedMemory(["a", "b"], EditMode.replace, "x", marker: "zzz");
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("not found"), e.msg);
    }
    assert(threw);
}

@("byContent not found errors out")
unittest {
    bool threw = false;
    try {
        editFileUnifiedMemory(["a", "b"], EditMode.replace, "x", searchContent: "zzz");
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("not found"), e.msg);
    }
    assert(threw);
}

@("replaceAll with byLine targeting is not supported")
unittest {
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

@("replaceAll with insert modes is not supported")
unittest {
    bool threw = false;
    try {
        editFileUnifiedMemory(["a"], EditMode.append, "x", marker: "a", replaceAll: true);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("replaceAll"), e.msg);
    }
    assert(threw);
}

@("matchIndex=2 targets the second occurrence with byMarker")
unittest {
    string[] fileLines = ["a", "b", "a", "c"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "x", marker: "a", matchIndex: 2);
    assert(res.lines == ["a", "b", "x", "c"], res.lines.to!string);
    assert(res.matched.matchedAt == 3, res.matched.matchedAt.to!string);
    assert(res.matched.matchedLines == 1);
}

@("matchIndex=3 targets the third occurrence with byContent")
unittest {
    string[] fileLines = ["foo", "bar", "foo", "bar", "foo", "bar", "end"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "X",
            searchContent: "foo\nbar", matchIndex: 3);
    assert(res.lines == ["foo", "bar", "foo", "bar", "X", "end"], res.lines.to!string);
    assert(res.matched.matchedAt == 5, res.matched.matchedAt.to!string);
    assert(res.matched.matchedLines == 2);
}

@("matchIndex out of range with byMarker reports the actual occurrence count")
unittest {
    bool threw = false;
    try {
        editFileUnifiedMemory(["a", "b", "a"], EditMode.replace, "x", marker: "a", matchIndex: 3);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("matchIndex=3") && e.msg.canFind("2 occurrences"), e.msg);
    }
    assert(threw);
}

@("matchIndex out of range with byContent reports the actual occurrence count")
unittest {
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

@("matchIndex greater than 1 combined with replaceAll is rejected")
unittest {
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

@("byLine replaceAll error takes precedence over a matchIndex violation")
unittest {
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

@("matchIndex is ignored with byLine targeting")
unittest {
    string[] fileLines = ["a", "b", "c"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "x",
            startLine: 2, count: 1, matchIndex: 2);
    assert(res.lines == ["a", "x", "c"], res.lines.to!string);
    assert(res.matched.matchedAt == 2);
}

@("scope finds a byMarker inside the scope range")
unittest {
    string[] fileLines = ["a", "target", "c", "d"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "x", marker: "target",
            scopeStart: 2, scopeEnd: 3);
    assert(res.lines == ["a", "x", "c", "d"], res.lines.to!string);
    assert(res.matched.matchedAt == 2, res.matched.matchedAt.to!string);
}

@("byMarker outside the scope range is not found with a scope-mentioning error")
unittest {
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

@("byMarker after scopeEnd is not found")
unittest {
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

@("scopeStart alone searches from that line to EOF")
unittest {
    string[] fileLines = ["target", "b", "c", "target2"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "x", marker: "target2",
            scopeStart: 4);
    assert(res.lines == ["target", "b", "c", "x"], res.lines.to!string);
    assert(res.matched.matchedAt == 4, res.matched.matchedAt.to!string);
}

@("scopeEnd alone searches from line 1 to that line")
unittest {
    string[] fileLines = ["a", "target", "c", "target2"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "x", marker: "target",
            scopeEnd: 2);
    assert(res.lines == ["a", "x", "c", "target2"], res.lines.to!string);
    assert(res.matched.matchedAt == 2, res.matched.matchedAt.to!string);
}

@("scope finds a byContent block inside the scope range")
unittest {
    string[] fileLines = ["foo", "bar", "baz", "end"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "X",
            searchContent: "bar\nbaz", scopeStart: 2, scopeEnd: 3);
    assert(res.lines == ["foo", "X", "end"], res.lines.to!string);
    assert(res.matched.matchedAt == 2, res.matched.matchedAt.to!string);
}

@("byContent block outside the scope range is not found with a scope-mentioning error")
unittest {
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

@("a block anchored inside the scope may extend past scopeEnd")
unittest {
    string[] fileLines = ["foo", "bar", "baz", "qux", "end"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "X",
            searchContent: "bar\nbaz\nqux", scopeStart: 2, scopeEnd: 2);
    assert(res.lines == ["foo", "X", "end"], res.lines.to!string);
    assert(res.matched.matchedAt == 2, res.matched.matchedAt.to!string);
    assert(res.matched.matchedLines == 3, res.matched.matchedLines.to!string);
}

@("scopeEnd beyond EOF clamps to the file end")
unittest {
    string[] fileLines = ["a", "target", "c"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "x", marker: "target",
            scopeStart: 1, scopeEnd: 100);
    assert(res.lines == ["a", "x", "c"], res.lines.to!string);
    assert(res.matched.matchedAt == 2);
}

@("scopeStart beyond EOF is not found with a scope-mentioning error")
unittest {
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

@("empty file with a scope is not found with a scope-mentioning error")
unittest {
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

@("scopeStart greater than scopeEnd is a validation error")
unittest {
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

@("scopeStart below 1 is a validation error")
unittest {
    bool threw = false;
    try {
        editFileUnifiedMemory(["a"], EditMode.replace, "x", marker: "a", scopeStart: 0);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("scopeStart 0 must be >= 1"), e.msg);
    }
    assert(threw);
}

@("scopeEnd below 1 is a validation error")
unittest {
    bool threw = false;
    try {
        editFileUnifiedMemory(["a"], EditMode.replace, "x", marker: "a", scopeEnd: 0);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("scopeEnd 0 must be >= 1"), e.msg);
    }
    assert(threw);
}

@("scope is ignored with byLine targeting")
unittest {
    string[] fileLines = ["a", "b", "c"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "x",
            startLine: 2, count: 1, scopeStart: 1, scopeEnd: 1);
    assert(res.lines == ["a", "x", "c"], res.lines.to!string);
    assert(res.matched.matchedAt == 2);
}

@("scope plus matchIndex targets the nth occurrence within the scope")
unittest {
    string[] fileLines = ["a", "a", "a", "a"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "x", marker: "a",
            matchIndex: 2, scopeStart: 2, scopeEnd: 4);
    assert(res.lines == ["a", "a", "x", "a"], res.lines.to!string);
    assert(res.matched.matchedAt == 3, res.matched.matchedAt.to!string);
}

@("scope plus matchIndex out of range counts only in-scope occurrences")
unittest {
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

@("scope plus replaceAll replaces only the in-scope occurrences")
unittest {
    string[] fileLines = ["a", "a", "a", "a", "a"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "x", marker: "a",
            replaceAll: true, scopeStart: 2, scopeEnd: 3);
    assert(res.lines == ["a", "x", "x", "a", "a"], res.lines.to!string);
    assert(res.matched.matchedAt == 2, res.matched.matchedAt.to!string);
    assert(res.operations == 2, res.operations.to!string);
}

@("scope plus replaceAll with no in-scope occurrences is not found")
unittest {
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

@("scope plus byContent replaceAll replaces only the in-scope blocks")
unittest {
    string[] fileLines = ["foo", "bar", "foo", "bar", "foo", "bar", "end"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "X",
            searchContent: "foo\nbar", replaceAll: true, scopeStart: 3, scopeEnd: 5);
    assert(res.lines == ["foo", "bar", "X", "X", "end"], res.lines.to!string);
    assert(res.operations == 2, res.operations.to!string); // anchors at lines 3 and 5
}

@("scope plus byContent replaceAll on an empty file is not found")
unittest {
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

@("scope plus append anchors inside the scope and inserts after it")
unittest {
    string[] fileLines = ["a", "target", "c", "target2"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.append, "x", marker: "target",
            scopeStart: 2, scopeEnd: 2);
    assert(res.lines == ["a", "target", "x", "c", "target2"], res.lines.to!string);
    assert(res.matched.matchedAt == 2, res.matched.matchedAt.to!string);
}

@("scope plus byContent matchIndex targets the nth block within the scope")
unittest {
    string[] fileLines = ["foo", "bar", "foo", "bar", "foo", "bar", "end"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "X",
            searchContent: "foo\nbar", matchIndex: 2, scopeStart: 3, scopeEnd: 7);
    assert(res.lines == ["foo", "bar", "foo", "bar", "X", "end"], res.lines.to!string);
    assert(res.matched.matchedAt == 5, res.matched.matchedAt.to!string);
    assert(res.matched.matchedLines == 2);
}

@("scope plus byMarker remove deletes only the in-scope line")
unittest {
    string[] fileLines = ["a", "marker", "c", "marker", "e"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.remove, "", marker: "marker",
            scopeStart: 2, scopeEnd: 2);
    assert(res.lines == ["a", "c", "marker", "e"], res.lines.to!string);
    assert(res.matched.matchedAt == 2, res.matched.matchedAt.to!string);
    assert(res.linesChanged == -1);
}

@("scope plus insert_before inserts before the in-scope marker")
unittest {
    string[] fileLines = ["a", "target", "c", "target2"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.insert_before, "x",
            marker: "target", scopeStart: 2, scopeEnd: 2);
    assert(res.lines == ["a", "x", "target", "c", "target2"], res.lines.to!string);
    assert(res.matched.matchedAt == 2, res.matched.matchedAt.to!string);
}

@("byMarker with explicit count greater than 1 may extend past scopeEnd per the anchor rule")
unittest {
    // The marker is at line 2 (in scope [2,2]) but targetCount=3 replaces
    // lines 2-4, which reach beyond the scope end. The content-line-count
    // auto-derive was removed (over-consumption hole), so multi-line-content
    // shapes pass an explicit count; the anchor rule (count extends past the
    // scope end) is unchanged. ===
    string[] fileLines = ["a", "target", "c", "d", "e"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "x\ny\nz",
            marker: "target", scopeStart: 2, scopeEnd: 2, count: 3);
    assert(res.lines == ["a", "x", "y", "z", "e"], res.lines.to!string);
    assert(res.matched.matchedAt == 2, res.matched.matchedAt.to!string);
    assert(res.matched.matchedLines == 3, res.matched.matchedLines.to!string);
    assert(!res.autoCountUsed);
}

@("matchIndex with append mode targets the nth occurrence")
unittest {
    string[] fileLines = ["a", "b", "a", "c"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.append, "x", marker: "a", matchIndex: 2);
    assert(res.lines == ["a", "b", "a", "x", "c"], res.lines.to!string);
    assert(res.matched.matchedAt == 3, res.matched.matchedAt.to!string);
}

@("matchIndex=1 default targets the first match")
unittest {
    string[] fileLines = ["a", "b", "a"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "x", marker: "a", matchIndex: 1);
    assert(res.lines == ["x", "b", "a"], res.lines.to!string);
    assert(res.matched.matchedAt == 1);
}

@("remove mode with non-empty content errors out")
unittest {
    bool threw = false;
    try {
        editFileUnifiedMemory(["a"], EditMode.remove, "not empty", marker: "a");
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("remove"), e.msg);
    }
    assert(threw);
}

@("byLine replace without a count errors out")
unittest {
    bool threw = false;
    try {
        editFileUnifiedMemory(["a", "b"], EditMode.replace, "x", startLine: 1);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("count"), e.msg);
    }
    assert(threw);
}

@("byLine append on an empty file inserts at the start")
unittest {
    auto res = editFileUnifiedMemory(cast(string[])[], EditMode.append, "a\nb", startLine: 1);
    assert(res.lines == ["a", "b"], res.lines.to!string);
    assert(res.linesChanged == 2);
}

@("marker diagnostics report the closest match with line, text, and searched lines")
unittest {
    auto diag = buildMarkerDiagnostic(["hello world", "foo bar"], "hello worl");
    assert(diag["closestMatch"]["atLine"].integer == 1, diag.toString);
    assert(diag["closestMatch"]["matchedText"].str == "hello worl", diag.toString);
    assert(diag["searchedLines"].integer == 2, diag.toString);
}

@("block diagnostics report the closest match with mismatch detail")
unittest {
    auto diag = buildBlockDiagnostic([
        "a", "int total = x + y + z;", "int avg = total / 3;", "b"
    ], "int total = x + y;\nint avg = total / 3;");
    assert(diag["closestMatch"]["atLine"].integer == 2, diag.toString);
    assert(diag["closestMatch"]["mismatchAt"].str.canFind("expected"), diag.toString);
    assert(diag["closestMatch"]["matchedLines"].array.length >= 1, diag.toString);
}

@("marker diagnostics respect scope with clamped effective range")
unittest {
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

@("block diagnostics respect scope with no closest match outside it")
unittest {
    auto diag = buildBlockDiagnostic(["foo", "bar", "baz", "end"], "foo\nbar",
            scopeStart: 3, scopeEnd: 4);
    assert(diag["scope"]["start"].integer == 3, diag.toString);
    assert(diag["searchedLines"].integer == 2, diag.toString);
    // No closestMatch: the anchor "foo" is not inside the scope
    assert(!("closestMatch" in diag), diag.toString);
}

@("escapeInvisibleChars renders tab as \\t and NBSP/BOM as \\uXXXX, leaving plain text raw")
unittest {
    assert(escapeInvisibleChars("a\tb") == "a\\tb");
    assert(escapeInvisibleChars("a\u00a0b") == "a\\u00a0b");
    assert(escapeInvisibleChars("\ufeffBOM") == "\\ufeffBOM");
    assert(escapeInvisibleChars("plain text") == "plain text");
    assert(escapeInvisibleChars("") == "");
}

@(
        "differOnlyByInvisibleChars is false for equal strings and visible differences, true for invisible-only ones")
unittest {
    assert(!differOnlyByInvisibleChars("same", "same"));
    assert(!differOnlyByInvisibleChars("alpha", "beta"));
    assert(!differOnlyByInvisibleChars("a\tb", "a\tc"));
    assert(differOnlyByInvisibleChars("x\u00a0y", "xy")); // NBSP-only
    assert(differOnlyByInvisibleChars("x\u200by", "xy")); // zero-width space
}

@("block diagnostics flag NBSP-only mismatches with the invisible-characters phrase")
unittest {
    auto diag = buildBlockDiagnostic(["a", "int total = x\u00a0+ y;", "b"], "int total = x + y;");
    const m = diag["closestMatch"]["mismatchAt"].str;
    assert(m.canFind("differ only by invisible characters"), diag.toString);
    assert(m.canFind("\\u00a0"), diag.toString); // escaped rendering
    assert(m.canFind("expected 'int total = x + y;'"), diag.toString);
    assert(m.canFind("but found 'int total = x\\u00a0+ y;'"), diag.toString);
}

@("block diagnostics for a real text mismatch carry no invisible-characters phrase")
unittest {
    auto diag = buildBlockDiagnostic([
        "int total = x + y;", "int avg = total / 2;"
    ], "int total = x + y;\nint avg = total / 3;");
    const m = diag["closestMatch"]["mismatchAt"].str;
    assert(m.canFind("expected"), diag.toString);
    assert(!m.canFind("invisible"), diag.toString);
}

@("byContent append anchors after the matched block")
unittest {
    string[] fileLines = ["a", "foo", "bar", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.append, "X", searchContent: "foo\nbar");
    assert(res.lines == ["a", "foo", "bar", "X", "b"], res.lines.to!string);
    assert(res.matched.matchedAt == 2);
}

@("byContent insert_after alias anchors after the block")
unittest {
    string[] fileLines = ["a", "foo", "bar", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.insert_after, "X",
            searchContent: "foo\nbar");
    assert(res.lines == ["a", "foo", "bar", "X", "b"], res.lines.to!string);
}

@("byContent append on a block ending at EOF appends at the end")
unittest {
    string[] fileLines = ["a", "foo", "bar"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.append, "X", searchContent: "foo\nbar");
    assert(res.lines == ["a", "foo", "bar", "X"], res.lines.to!string);
}

@("byContent replaceAll rejects an explicit count")
unittest {
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

@("autoCountUsed is set for byContent auto-derived and replaceAll counts, not explicit ones")
unittest {
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

@("explicit startLine 0 is invalid, not treated as absent")
unittest {
    bool threw = false;
    try {
        editFileUnifiedMemory(["a", "b"], EditMode.replace, "x", startLine: 0);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("startLine") && e.msg.canFind("> 0"), e.msg);
    }
    assert(threw);
}

@("explicit startLine 0 with a marker is ambiguous, not ignored")
unittest {
    bool threw = false;
    try {
        editFileUnifiedMemory(["a"], EditMode.replace, "x", startLine: 0, marker: "a");
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("ambiguous"), e.msg);
    }
    assert(threw);
}

@("matchIndex 0 is invalid")
unittest {
    bool threw = false;
    try {
        editFileUnifiedMemory(["a"], EditMode.replace, "x", marker: "a", matchIndex: 0);
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("matchIndex") && e.msg.canFind(">= 1"), e.msg);
    }
    assert(threw);
}

@("byLine remove deletes the line range")
unittest {
    string[] fileLines = ["a", "b", "c", "d", "e"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.remove, "", startLine: 2, count: 2);
    assert(res.lines == ["a", "d", "e"], res.lines.to!string);
    assert(res.matched.matchedAt == 2);
    assert(res.matched.matchedLines == 2);
    assert(res.linesChanged == -2, res.linesChanged.to!string);
    assert(res.operations == 1);
    assert(!res.autoCountUsed);
}

@("byLine insert_before inserts content before the line")
unittest {
    string[] fileLines = ["a", "b", "c"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.insert_before, "X\nY", startLine: 2);
    assert(res.lines == ["a", "X", "Y", "b", "c"], res.lines.to!string);
    assert(res.linesChanged == 2, res.linesChanged.to!string);
    assert(res.matched.matchedAt == 2);
    assert(res.matched.matchedLines == 1);
}

@("byLine insert_after inserts content after the line")
unittest {
    string[] fileLines = ["a", "b", "c"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.insert_after, "X", startLine: 2);
    assert(res.lines == ["a", "b", "X", "c"], res.lines.to!string);
    assert(res.linesChanged == 1, res.linesChanged.to!string);
    assert(res.matched.matchedAt == 2);
}

@(
        "byMarker remove with a single-line marker and no count auto-derives to 1 without the auto-count flag")
unittest {
    // === REMOVE mode is unchanged by the auto-count rule: it has no
    // content-line-count auto-derive branch (and requires empty content), so a
    // single-line marker with remove and count<1 falls through to targetCount=1
    // without setting autoCountUsed. ===
    string[] fileLines = ["a", "marker", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.remove, "", marker: "marker");
    assert(res.lines == ["a", "b"], res.lines.to!string);
    assert(res.linesChanged == -1, res.linesChanged.to!string);
    assert(res.matched.matchedAt == 2);
    assert(res.matched.matchedLines == 1);
    assert(!res.autoCountUsed);
    assert(res.note.empty);
}

@("byMarker insert_before inserts content before the marker line")
unittest {
    string[] fileLines = ["a", "marker", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.insert_before, "X", marker: "marker");
    assert(res.lines == ["a", "X", "marker", "b"], res.lines.to!string);
    assert(res.matched.matchedAt == 2);
    assert(res.matched.matchedLines == 1);
}

@("byMarker insert_after inserts content after the marker line")
unittest {
    string[] fileLines = ["a", "marker", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.insert_after, "X", marker: "marker");
    assert(res.lines == ["a", "marker", "X", "b"], res.lines.to!string);
    assert(res.matched.matchedAt == 2);
    assert(res.matched.matchedLines == 1);
}

@("duplicate markers target the first by default")
unittest {
    string[] fileLines = ["x", "marker", "marker", "z"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "X", marker: "marker");
    assert(res.lines == ["x", "X", "marker", "z"], res.lines.to!string);
    assert(res.matched.matchedAt == 2, res.matched.matchedAt.to!string);
}

@("duplicate blocks target the first by default")
unittest {
    string[] fileLines = ["a", "foo", "b", "foo", "c"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace, "X", searchContent: "foo");
    assert(res.lines == ["a", "X", "b", "foo", "c"], res.lines.to!string);
    assert(res.matched.matchedAt == 2, res.matched.matchedAt.to!string);
}

@("empty file with byMarker is not found")
unittest {
    bool threw = false;
    try {
        editFileUnifiedMemory(cast(string[])[], EditMode.replace, "x", marker: "m");
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("not found"), e.msg);
    }
    assert(threw);
}

@("empty file with byContent is not found")
unittest {
    bool threw = false;
    try {
        editFileUnifiedMemory(cast(string[])[], EditMode.replace, "x", searchContent: "m");
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("not found"), e.msg);
    }
    assert(threw);
}

@("empty file with byLine insert_before creates content at the start")
unittest {
    auto res = editFileUnifiedMemory(cast(string[])[], EditMode.insert_before, "a", startLine: 1);
    assert(res.lines == ["a"], res.lines.to!string);
    assert(res.linesChanged == 1, res.linesChanged.to!string);
}

@("single-line file byMarker replace with single-line content works")
unittest {
    auto res = editFileUnifiedMemory(["only"], EditMode.replace, "X", marker: "only");
    assert(res.lines == ["X"], res.lines.to!string);
    assert(!res.autoCountUsed);
    assert(res.matched.matchedAt == 1);
}

@(
        "single-line file byMarker replace with multi-line content and no count errors with auto-count guidance")
unittest {
    // === Same hard-error as the larger files: the auto-count block runs
    // before the EOF check, so the guidance fires on a single-line file too. ===
    bool threw = false;
    try {
        editFileUnifiedMemory(["only"], EditMode.replace, "X\nY", marker: "only");
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("marker matches 1 line but content has 2 lines"), e.msg);
        assert(e.msg.canFind("pass count=2"), e.msg);
    }
    assert(threw, "over-consumption shape must hard-error");
}

@("single-line file byContent replace works")
unittest {
    auto res = editFileUnifiedMemory(["only"], EditMode.replace, "X", searchContent: "only");
    assert(res.lines == ["X"], res.lines.to!string);
    assert(res.matched.matchedAt == 1);
    assert(res.matched.matchedLines == 1);
}

@("single-line file byLine remove clears the file")
unittest {
    auto res = editFileUnifiedMemory(["only"], EditMode.remove, "", startLine: 1, count: 1);
    assert(res.lines.length == 0, res.lines.to!string);
    assert(res.linesChanged == -1, res.linesChanged.to!string);
}

@("multi-line content is preserved exactly through the unified path")
unittest {
    string[] fileLines = ["a", "marker", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.replace,
            "  keep\tindent\n\t\ttab", marker: "marker", count: 1);
    assert(res.lines == ["a", "  keep\tindent", "\t\ttab", "b"], res.lines.to!string);
    assert(!res.autoCountUsed);
}

@("byContent append with explicit count still anchors after the whole block")
unittest {
    string[] fileLines = ["a", "foo", "bar", "baz", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.append, "X",
            searchContent: "foo\nbar\nbaz", count: 1);
    assert(res.lines == ["a", "foo", "bar", "baz", "X", "b"], res.lines.to!string);
    assert(res.matched.matchedLines == 3, res.matched.matchedLines.to!string);
}

@("byContent insert_before with explicit count anchors before the block")
unittest {
    string[] fileLines = ["a", "foo", "bar", "baz", "b"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.insert_before, "X",
            searchContent: "foo\nbar\nbaz", count: 1);
    assert(res.lines == ["a", "X", "foo", "bar", "baz", "b"], res.lines.to!string);
    assert(res.matched.matchedLines == 3, res.matched.matchedLines.to!string);
}

@("byMarker insert modes ignore an explicit count")
unittest {
    string[] fileLines = ["a", "marker", "b", "c"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.append, "X", marker: "marker", count: 3);
    assert(res.lines == ["a", "marker", "X", "b", "c"], res.lines.to!string);
    assert(res.matched.matchedLines == 1, res.matched.matchedLines.to!string);
}

@("byLine insert modes ignore an explicit count")
unittest {
    string[] fileLines = ["a", "b", "c"];
    auto res = editFileUnifiedMemory(fileLines, EditMode.insert_after, "X",
            startLine: 2, count: 3);
    assert(res.lines == ["a", "b", "X", "c"], res.lines.to!string);
    assert(res.matched.matchedLines == 1);
}

@("multi-hunk diff preserves the lines between hunks")
unittest {
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

@("single-hunk diff with context and one replacement applies without warnings")
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

@("single-hunk diff with context and one deletion applies without warnings")
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

@("context-only hunk applies with no changes")
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

@("deletion-only hunk without context lines applies")
unittest {
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

@("removal lines past EOF error instead of silently succeeding")
unittest {
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

@("multiple additions in one hunk apply")
unittest {
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

@("advisory header counts: old and new mismatches apply with warnings")
unittest {
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

@("auto-count: declared 5 old lines but body has 6 warns and uses the body count")
unittest {
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

@("auto-count: declared 3 new lines but body has 4 warns and uses the body count")
unittest {
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

@("multi-hunk mix of correct and wrong counts applies all hunks with per-hunk warnings")
unittest {
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

@("fuzzy matching tolerates extra leading space in context lines")
unittest {
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

@("fuzzy matching tolerates trailing whitespace in context lines")
unittest {
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

@("fuzzy matching preserves file lines and added lines verbatim")
unittest {
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

@("fuzzy=false rejects whitespace differences in context lines")
unittest {
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

@("fuzzy=false with exact matches applies normally")
unittest {
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

@("default fuzzy matching tolerates indentation drift in a real edit")
unittest {
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

@("unifiedDiffLines produces the exact golden diff for a fixed replacement")
unittest {
    // dfmt off
    const oldLines = ["a", "b", "c", "d", "e", ""];
    const newLines = ["a", "b", "c", "d", "e", "f"];
    assert(unifiedDiffLines(oldLines, newLines) == q{@@ -3,4 +3,4 @@
 c
 d
 e
-
+f}, unifiedDiffLines(oldLines, newLines));
    // dfmt on
}

@("unifiedDiffLines for a new file shows an empty old range in the header")
unittest {
    // dfmt off
    assert(unifiedDiffLines([], ["one", "two"]) == q{@@ -0,0 +1,2 @@
+one
+two}, unifiedDiffLines([], ["one", "two"]));
    // dfmt on
}

@("unifiedDiffLines for an emptied file shows an empty new range in the header")
unittest {
    // dfmt off
    assert(unifiedDiffLines(["one", "two"], []) == q{@@ -1,2 +0,0 @@
-one
-two}, unifiedDiffLines(["one", "two"], []));
    // dfmt on
}

@("unifiedDiffLines splits changes separated by a long common run into two hunks")
unittest {
    // dfmt off
    const oldLines = ["a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l"];
    const newLines = ["a", "B", "c", "d", "e", "f", "g", "h", "i", "J", "k", "l"];
    assert(unifiedDiffLines(oldLines, newLines) == q{@@ -1,5 +1,5 @@
 a
-b
+B
 c
 d
 e
@@ -7,6 +7,6 @@
 g
 h
 i
-j
+J
 k
 l}, unifiedDiffLines(oldLines, newLines));
    // dfmt on
}

@("unifiedDiffLines output round-trips through applyDiffMemory for one hunk")
unittest {
    const oldLines = ["a", "b", "c", "d", "e", "f", "g", "h"];
    const newLines = ["a", "X", "Y", "d", "e", "f", "g", "Z"];
    const diffText = unifiedDiffLines(oldLines, newLines);
    const result = applyDiffMemory(oldLines.dup, diffText.splitLines);
    assert(result.warnings.empty, result.warnings.to!string);
    assert(result.lines == newLines, result.lines.to!string);
}

@("unifiedDiffLines output round-trips through applyDiffMemory for two hunks")
unittest {
    const oldLines = [
        "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l"
    ];
    const newLines = [
        "a", "B", "c", "d", "e", "f", "g", "h", "i", "J", "k", "l"
    ];
    const diffText = unifiedDiffLines(oldLines, newLines);
    const result = applyDiffMemory(oldLines.dup, diffText.splitLines);
    assert(result.warnings.empty, result.warnings.to!string);
    assert(result.lines == newLines, result.lines.to!string);
}

@("unifiedDiffLines output round-trips with empty removed and added lines")
unittest {
    const oldLines = ["a", "", "b"];
    const newLines = ["a", "", "c"];
    const diffText = unifiedDiffLines(oldLines, newLines);
    const result = applyDiffMemory(oldLines.dup, diffText.splitLines);
    assert(result.warnings.empty, result.warnings.to!string);
    assert(result.lines == newLines, result.lines.to!string);
}

@("unifiedDiffLines new-file hunk is not accepted by applyDiffMemory")
unittest {
    // The @@ -0,0 form describes a file with no pre-edit lines; applyDiffMemory
    // requires at least one pre-edit line, so it throws instead of accepting it.
    const diffText = unifiedDiffLines([], ["one", "two"]);
    assert(diffText == "@@ -0,0 +1,2 @@\n+one\n+two", diffText);
    import std.exception : collectException;

    const ex = collectException(applyDiffMemory([], diffText.splitLines));
    assert(ex !is null, "expected applyDiffMemory to reject a new-file hunk");
}

@("unifiedDiffLines is deterministic")
unittest {
    const oldLines = ["x", "y", "z"];
    const newLines = ["x", "w", "z"];
    assert(unifiedDiffLines(oldLines, newLines) == unifiedDiffLines(oldLines, newLines));
}

@("integration: editFile byLine replace writes the file and returns match metadata")
unittest {
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

@("integration: editFile byMarker replace rewrites the marker line")
unittest {
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

@("integration: editFile byMarker remove deletes the marker line")
unittest {
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

@("integration: editFile byMarker insert_before inserts before the marker line")
unittest {
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

@("integration: editFile byMarker insert_after inserts after the marker line")
unittest {
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

@("integration: editFile byMarker with multi-line content requires an explicit count")
unittest {
    // === The content-line-count auto-derive was removed (over-consumption
    // hole): a single-line marker with multi-line replace content and no
    // count now errors at the tool level with auto-count guidance. ===
    auto ctx = makeTestContext("edit_marker_autocount");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nmarker\nline3\nline4\nline5\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt", content: "newA\nnewB\nnewC",
            mode: "replace", marker: "marker", count: 3));
    assert(result.success, result.msg);

    auto json = parseJSON(result.msg);
    assert(json["matchedLines"].integer == 3, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "line1\nnewA\nnewB\nnewC\nline5\n", content);
}

@("integration: editFile byMarker explicit count=1 replaces only the marker line")
unittest {
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

@(
        "integration: editFile byMarker with multi-line content and no count errors with auto-count guidance")
unittest {
    // === Tool-level mirror of the engine-level hard-error: the auto-count
    // block runs before any EOF check, so the guidance fires instead of a
    // generic exceeds-file-length error. ===
    auto ctx = makeTestContext("edit_marker_autocount_oob");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nmarker\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "newA\nnewB\nnewC", mode: "replace", marker: "marker"));
    assert(!result.success, result.msg);
    assert(result.msg.canFind("marker matches 1 line but content has 3 lines"), result.msg);
    assert(result.msg.canFind("pass count=3"), result.msg);
}

@("integration: editFile byMarker not found returns a failure JSON with diagnostic")
unittest {
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

@("integration: editFile empty marker is a targeting error")
unittest {
    auto ctx = makeTestContext("edit_marker_empty");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "x", mode: "replace", marker: ""));
    assert(!result.success);
    assert(result.msg.canFind("targeting"), result.msg);
}

@("integration: editFile ambiguous targeting is an error")
unittest {
    auto ctx = makeTestContext("edit_ambiguous");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "x", mode: "replace", startLine: 1, marker: "line1"));
    assert(!result.success);
    assert(result.msg.canFind("ambiguous"), result.msg);
}

@("integration: editFile byContent replace rewrites the first matching block only")
unittest {
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

@("integration: editFile byContent matches through trimmed equality across indentation")
unittest {
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

@("integration: editFile byContent not found returns a failure JSON with diagnostic")
unittest {
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

@("integration: editFile empty searchContent is a targeting error")
unittest {
    auto ctx = makeTestContext("edit_content_empty");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "replacement", mode: "replace", searchContent: ""));
    assert(!result.success);
    assert(result.msg.canFind("targeting"), result.msg);
}

@("integration: editFile byContent replaceAll rewrites every occurrence")
unittest {
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

@("integration: editFile byMarker replaceAll rewrites every marker line")
unittest {
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

@("integration: editFile byContent replaceAll with zero matches is an error")
unittest {
    auto ctx = makeTestContext("edit_content_all_zero");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nline2\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt", content: "replacement",
            mode: "replace", searchContent: "notfound", replaceAll: true));
    assert(!result.success);
    assert(result.msg.canFind("not found"), result.msg);
}

@("integration: editFile replaceAll with byLine is not supported")
unittest {
    auto ctx = makeTestContext("edit_byline_all");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "x", mode: "replace", startLine: 1, count: 1, replaceAll: true));
    assert(!result.success);
    assert(result.msg.canFind("replaceAll"), result.msg);
}

@("integration: editFile dryRun leaves the file untouched and returns the preview")
unittest {
    auto ctx = makeTestContext("edit_dryrun");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nhello world\nline3\n");

    string original = readTestFile(ctx, "test.txt");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt", content: "replaced",
            mode: "replace", marker: "hello world", dryRun: true));
    assert(result.success, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == original, content);

    auto json = parseJSON(result.msg);
    assert(json["preview"].str.canFind("replaced"), result.msg);
    assert(json["matchedAt"].integer == 2, result.msg);
}

@("integration: editFile dryRun with multi-line content previews all inserted lines")
unittest {
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

@("integration: editFile byLine dryRun previews without modifying the file")
unittest {
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

@("integration: editFile includeDiff returns a unified diff of the edit")
unittest {
    auto ctx = makeTestContext("edit_includediff");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "a\nb\nc\nd\ne\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt", content: "X",
            mode: "replace", startLine: 2, count: 1, includeDiff: true));
    assert(result.success, result.msg);

    auto json = parseJSON(result.msg);
    assert(json["ok"].boolean, result.msg);
    const diff = json["diff"].str;
    assert(diff.canFind("@@"), result.msg);
    assert(diff.canFind("-b"), result.msg);
    assert(diff.canFind("+X"), result.msg);
}

@("integration: editFile includeDiff with dryRun previews the same diff")
unittest {
    auto ctx = makeTestContext("edit_includediff_dryrun");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "a\nb\nc\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt", content: "X",
            mode: "replace", startLine: 2, count: 1, includeDiff: true, dryRun: true));
    assert(result.success, result.msg);

    auto json = parseJSON(result.msg);
    assert(json["diff"].str == "@@ -1,3 +1,3 @@\n a\n-b\n+X\n c", json["diff"].str);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "a\nb\nc\n", content);
}

@("integration: editFile without includeDiff has no diff field in the result")
unittest {
    auto ctx = makeTestContext("edit_no_includediff");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "a\nb\nc\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "X", mode: "replace", startLine: 2, count: 1));
    assert(result.success, result.msg);

    auto json = parseJSON(result.msg);
    assert(json["ok"].boolean, result.msg);
    assert("diff" !in json, result.msg);
}

@("integration: editFile noOp with includeDiff returns an empty diff string")
unittest {
    // A no-op edit has no changed lines, so the unified diff is empty even
    // when includeDiff asks for one; the field is still present.
    auto ctx = makeTestContext("edit_noop_includediff");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "a\nb\nc\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt", content: "b\n",
            mode: "replace", startLine: 2, count: 1, includeDiff: true));
    assert(result.success, result.msg);

    auto json = parseJSON(result.msg);
    assert(json["ok"].boolean, json.toString);
    assert(json["noOp"].boolean, json.toString);
    assert("diff" in json, json.toString);
    assert(json["diff"].str == "", json.toString);
    assert(readTestFile(ctx, "test.txt") == "a\nb\nc\n", "no-op never writes");
}

@("integration: editFile byLine append inserts after the line")
unittest {
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

@("integration: editFile byContent explicit count extends the replaced range")
unittest {
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

@("integration: matchIndex=2 replaces the second occurrence")
unittest {
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

@(
        "integration: matchIndex out of range reports the occurrence count with diagnostic and suggestion")
unittest {
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

@("integration: byContent matchIndex out of range reports the occurrence count in the JSON error")
unittest {
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

@("integration: matchIndex greater than 1 with replaceAll is rejected")
unittest {
    auto ctx = makeTestContext("edit_matchindex_replaceall");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "a\na\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt", content: "x",
            mode: "replace", marker: "a", replaceAll: true, matchIndex: 2));
    assert(!result.success);
    assert(result.msg.canFind("matchIndex") && result.msg.canFind("replaceAll"), result.msg);
}

@("integration: matchIndex=2 replaces the second block with byContent")
unittest {
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

@("integration: matchIndex is ignored with byLine targeting")
unittest {
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

@("integration: byMarker targets the first of duplicate markers")
unittest {
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

@("integration: empty file byLine append creates content preserving the no-trailing-newline state")
unittest {
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

@("integration: single-line file byLine replace works")
unittest {
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

@("integration: trailing-newline state is preserved and the dryRun preview matches exact bytes")
unittest {
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

@("integration: byContent append inserts after the whole matched block")
unittest {
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

@("integration: append with no targeting appends at the end preserving the trailing newline")
unittest {
    auto ctx = makeTestContext("edit_append_no_targeting");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "a\nb\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "X", mode: "append"));
    assert(result.success, result.msg);

    auto json = parseJSON(result.msg);
    assert(json["ok"].type == JSONType.true_, result.msg);
    assert(json["matchedAt"].integer == 3, result.msg);
    assert(json["matchedLines"].integer == 1, result.msg);
    assert(json["linesChanged"].integer == 1, result.msg);
    assert(json["operations"].integer == 1, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "a\nb\nX\n", content);
}

@("editFile linesChanged field")
unittest {
    // Integration test: linesChanged is the signed net line-count change (new
    // total minus old total) in the success JSON for grow (positive), shrink
    // (negative), and no-op (0) edits; the value comes from the actual line
    // counts, not the engine's own accounting.
    auto ctx = makeTestContext("edit_lineschanged");
    scope (exit)
        ctx.teardown;

    // Grow: 2 lines added -> linesChanged = +2.
    createTestFile(ctx, "grow.txt", "a\nb\n");
    auto result = editFile(ctx, UnifiedEditFileParams(path: "grow.txt",
            content: "x\ny", mode: "append"));
    assert(result.success, result.msg);
    auto json = parseJSON(result.msg);
    assert(json["linesChanged"].integer == 2, result.msg);
    assert(json["matchedAt"].integer == 3, result.msg);
    assert(json["matchedLines"].integer == 1, result.msg);
    assert(json["operations"].integer == 1, result.msg);

    // Shrink: 3 lines removed -> linesChanged = -3.
    createTestFile(ctx, "shrink.txt", "a\nb\nc\nd\ne\nf\n");
    result = editFile(ctx, UnifiedEditFileParams(path: "shrink.txt", content: "",
            mode: "remove", startLine: 2, count: 3));
    assert(result.success, result.msg);
    json = parseJSON(result.msg);
    assert(json["linesChanged"].integer == -3, result.msg);

    // No-op: replace with identical content -> linesChanged = 0.
    createTestFile(ctx, "noop.txt", "a\nb\nc\n");
    result = editFile(ctx, UnifiedEditFileParams(path: "noop.txt", content: "b",
            mode: "replace", startLine: 2, count: 1));
    assert(result.success, result.msg);
    json = parseJSON(result.msg);
    assert(json["linesChanged"].integer == 0, result.msg);
}

@("integration: byContent replaceAll with an explicit count is rejected")
unittest {
    auto ctx = makeTestContext("edit_content_all_count");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "foo\nbar\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt", content: "x",
            mode: "replace", searchContent: "foo", count: 2, replaceAll: true));
    assert(!result.success);
    assert(result.msg.canFind("count"), result.msg);
}

@("integration: matchIndex 0 is invalid")
unittest {
    auto ctx = makeTestContext("edit_matchindex_zero");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "a\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "x", mode: "replace", marker: "a", matchIndex: 0));
    assert(!result.success);
    assert(result.msg.canFind("matchIndex"), result.msg);
}

@(
        "integration: applyDiff fuzzy matching tolerates whitespace in dryRun, file unmodified, preview returned")
unittest {
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

@(
        "integration: applyDiff fuzzy=false fails a whitespace-differing context line with Context mismatch")
unittest {
    auto ctx = makeTestContext("difffuzzy_exact");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "  indented line1\n  indented line2\n");

    string diff = "--- a/test.txt\n+++ b/test.txt\n@@ -1,2 +1,2 @@\n indented line1\n indented line2\n";

    auto result = applyDiff(ctx, ApplyDiffParams("test.txt", diff, fuzzy: false));
    assert(!result.success, result.msg);
    assert(result.msg.canFind("Context mismatch"), result.msg);
}

@(
        "integration: applyDiff dryRun does not modify the file, preview shows the edit, linesChanged is 0")
unittest {
    auto ctx = makeTestContext("diffdryrun");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nline2\nline3\n");

    string original = readTestFile(ctx, "test.txt");

    string diff = "--- a/test.txt\n+++ b/test.txt\n@@ -1,3 +1,3 @@\n line1\n-line2\n+line2modified\n line3\n";

    auto result = applyDiff(ctx, ApplyDiffParams("test.txt", diff, dryRun: true));
    assert(result.success, result.msg);

    auto content = readTestFile(ctx, "test.txt");
    assert(content == original, content);

    auto json = parseJSON(result.msg);
    assert(json["preview"].str.canFind("line2modified"), json.toString);
    assert(!json["preview"].str.canFind("-line2"), json.toString);
    assert(json["warnings"].array.empty, "correct counts should produce no warnings");
    assert(json["hunksApplied"].integer == 1, "expected 1 hunk applied");
    assert(json["linesChanged"].integer == 0, "expected 0 lines changed");
}

@("integration: applyDiff with wrong hunk counts applies with old-lines and new-lines warnings")
unittest {
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

    auto content = readTestFile(ctx, "test.txt");
    assert(content == "line1a\nline1b\nline1c\nline3\nline4\n", content);
}

@("integration: applyDiff with an empty diff errors")
unittest {
    auto ctx = makeTestContext("diffdryrun_empty");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\n");

    auto result = applyDiff(ctx, ApplyDiffParams("test.txt", "", dryRun: true));
    assert(!result.success);
    assert(result.msg.canFind("empty"), result.msg);
}

@("integration: workarea confinement rejects an absolute path")
unittest {
    auto ctx = makeTestContext("workarea");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "/etc/passwd",
            content: "x", mode: "replace", marker: "marker"));
    assert(!result.success);
    assert(result.msg.canFind("absolute"), result.msg);
}

@("integration: workarea confinement rejects path traversal")
unittest {
    auto ctx = makeTestContext("workarea_traversal");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "../test.txt",
            content: "x", mode: "replace", marker: "marker"));
    assert(!result.success);
    assert(result.msg.canFind("workarea") || result.msg.canFind("outside"), result.msg);
}

@("integration: editFile with an invalid mode errors")
unittest {
    auto ctx = makeTestContext("invalidmode");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "x", mode: "invalid-mode", marker: "line1"));
    assert(!result.success);
    assert(result.msg.canFind("invalid"), result.msg);
}

@("integration: editFile with whitespace-only searchContent errors")
unittest {
    auto ctx = makeTestContext("whitespaceonly");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "replacement", mode: "replace", searchContent: "   \n\n   "));
    assert(!result.success);
    assert(result.msg.canFind("non-empty"), result.msg);
}

@("integration: editFile remove mode with non-empty content errors")
unittest {
    auto ctx = makeTestContext("remove_content");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "not empty", mode: "remove", marker: "line1"));
    assert(!result.success);
    assert(result.msg.canFind("remove"), result.msg);
}

@("integration: editFile with a nonexistent path errors")
unittest {
    auto ctx = makeTestContext("missing_path");
    scope (exit)
        ctx.teardown;

    auto result = editFile(ctx, UnifiedEditFileParams(path: "nonexistent.txt",
            content: "x", mode: "replace", marker: "marker"));
    assert(!result.success);
}

@("integration: editFile scope finds a byMarker within the scope")
unittest {
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

@("integration: editFile scope not-found outside the scope mentions the scope in the error and diagnostic, file untouched")
unittest {
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

@("integration: editFile scope replaces the in-scope byContent block")
unittest {
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

@("integration: editFile scope is ignored for byLine")
unittest {
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

@("integration: editFile scope inverted range is a validation error")
unittest {
    auto ctx = makeTestContext("scope_invalid");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "a\nb\nc\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "X", mode: "replace", marker: "b", scopeStart: 5, scopeEnd: 2));
    assert(!result.success);
    assert(result.msg.canFind("scopeStart"), result.msg);
}

@("integration: editFile scope plus dryRun previews without modifying the file")
unittest {
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

@("byLine with verifyContent match: the edit proceeds and linesChanged reflects the replacement")
unittest {
    string[] fileLines = ["header", "line1", "line2", "line3", "footer"];
    auto outcome = executeByLine(fileLines, EditMode.replace, "new", 2, 2, false, "line1\nline2");
    assert(outcome.lines == ["header", "new", "line3", "footer"], outcome.lines.to!string);
    assert(outcome.linesChanged == -1); // 1 content line replaces 2 lines
}

@("byLine with verifyContent mismatch throws with the actual and expected content in the error")
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

@(
        "byLine verifyContent mismatch differing only by invisible characters appends the invisible-char note")
unittest {
    string[] fileLines = ["header", "li\u00a0ne1", "line2", "line3", "footer"];
    bool threw = false;
    try {
        executeByLine(fileLines, EditMode.replace, "new", 2, 1, false, "line1");
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("verifyContent mismatch"), e.msg);
        assert(e.msg.canFind("differ only by invisible characters"), e.msg);
        assert(e.msg.canFind("\\u00a0"), e.msg); // escaped rendering in the note
    }
    assert(threw);
}

@("byLine verifyContent mismatch with visible differences has no invisible-char note")
unittest {
    string[] fileLines = ["header", "line1", "line2", "line3", "footer"];
    bool threw = false;
    try {
        executeByLine(fileLines, EditMode.replace, "new", 2, 1, false, "wrong content");
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("verifyContent mismatch"), e.msg);
        assert(!e.msg.canFind("invisible"), e.msg);
    }
    assert(threw);
}

@("byLine verifyContent with startLine past the file end throws beyond end of file")
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

@("linesChanged in editFileUnifiedMemory outcome")
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

@("empty-line symmetric matching: a file blank line is skipped when absent from the search")
unittest {
    string[] fileLines = ["foo", "", "bar"];
    string[] searchLines = ["foo", "bar"];
    auto result = findCodeBlock(fileLines, searchLines);
    assert(result.found);
    assert(result.start == 0);
    assert(result.end == 3); // range includes the empty line
}

@("empty-line symmetric matching: a search blank line is skipped when absent from the file")
unittest {
    string[] fileLines = ["foo", "bar"];
    string[] searchLines = ["foo", "", "bar"];
    auto result = findCodeBlock(fileLines, searchLines);
    assert(result.found);
    assert(result.start == 0);
    assert(result.end == 2);
}

@("empty-line symmetric matching: blank lines in both match")
unittest {
    string[] fileLines = ["foo", "", "bar"];
    string[] searchLines = ["foo", "", "bar"];
    auto result = findCodeBlock(fileLines, searchLines);
    assert(result.found);
    assert(result.start == 0);
    assert(result.end == 3);
}

// === empty-line symmetric matching — asymmetric blanks ===
@("empty-line symmetric matching: asymmetric blank lines are all skipped")
unittest {
    string[] fileLines = ["foo", "", "", "bar"];
    string[] searchLines = ["foo", "", "bar"];
    auto result = findCodeBlock(fileLines, searchLines);
    assert(result.found);
    assert(result.start == 0);
    assert(result.end == 4); // range includes both empty lines
}

@("empty-line symmetric matching: a non-empty mismatch after skipped blanks fails")
unittest {
    string[] fileLines = ["foo", "", "wrong", "bar"];
    string[] searchLines = ["foo", "bar"];
    auto result = findCodeBlock(fileLines, searchLines);
    assert(!result.found); // "wrong" != "bar" — no false match
}

@("multi-line marker success: a 2-line marker is replaced with auto-count and matchedLines 2")
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

@(
        "multi-line marker partial fail: a mismatching second line continues the search and ends not found")
unittest {
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

@("multi-line marker not found: the first anchor line is never found")
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

@("multi-line marker replaceAll replaces only the full-anchor positions")
unittest {
    // Three "foo" lines, but only two have the matching second line "bar".
    // The middle "foo" has "baz" as the next line, so it is skipped.
    string[] fileLines = ["foo", "bar", "foo", "baz", "foo", "bar", "end"];
    auto outcome = editFileUnifiedMemory(fileLines, EditMode.replace, "X\nY",
            marker: "foo\nbar", replaceAll: true);
    assert(outcome.lines == ["X", "Y", "foo", "baz", "X", "Y", "end"], outcome.lines.to!string);
    assert(outcome.operations == 2);
}

@("multi-line marker with scope limits the anchor search to the scope")
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

@("multi-line marker over 20 lines is rejected")
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

@("auto-count note describes the marker line count auto-count")
unittest {
    string[] fileLines = ["a", "step1", "    // details", "b"];
    auto outcome = editFileUnifiedMemory(fileLines, EditMode.replace,
            "new line", marker: "step1\n    // details");
    assert(outcome.autoCountUsed);
    assert(outcome.note.canFind("marker line count"), outcome.note);
    assert(outcome.matched.matchedLines == 2);
}

@(
        "byMarker replace with single-line marker and multi-line content without count errors with auto-count guidance")
unittest {
    // === The over-consumption shape (single-line marker + multi-line
    // content + count<1) used to auto-derive count from the CONTENT line
    // count, consuming lines the agent never targeted; it now hard-errors
    // with guidance to pass an explicit count or use a multi-line marker. ===
    string[] fileLines = ["a", "marker", "old1", "old2", "b"];
    bool threw = false;
    try {
        editFileUnifiedMemory(fileLines, EditMode.replace, "new1\nnew2", marker: "marker");
    } catch (Exception e) {
        threw = true;
        assert(e.msg.canFind("marker matches 1 line but content has 2 lines"), e.msg);
        assert(e.msg.canFind("pass count=2"), e.msg);
        assert(e.msg.canFind("multi-line marker"), e.msg);
    }
    assert(threw, "over-consumption shape must hard-error");
}

@("byLine without count errors with a suggestion to use byMarker or byContent")
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

@("multi-line marker with an explicit count has autoCountUsed false and no note")
unittest {
    string[] fileLines = ["a", "step1", "    // details", "extra", "b"];
    auto outcome = editFileUnifiedMemory(fileLines, EditMode.replace, "X\nY",
            marker: "step1\n    // details", count: 3);
    assert(!outcome.autoCountUsed);
    assert(outcome.note.empty);
    assert(outcome.matched.matchedLines == 3);
}

// Direct state-machine tests below use TestFileContext (which mixes in
// FileDirtyTracking); the end-to-end tests drive the tools themselves so the
// observable behavior (dirty enforcement, error message, clearing) is covered.

@("a fresh context starts with empty dirty-tracking state")
unittest {
    auto ctx = makeTestContext("dirty_fresh_empty");
    scope (exit)
        ctx.teardown;

    import llm.tool_call.utility : pathToWorkarea;

    auto p = pathToWorkarea(ctx, "a.md").path;
    assert(!ctx.isDirty(p), "fresh context: nothing is dirty");
    assert(ctx.observedHash(p) == -1, "fresh context: no snapshot recorded");
}

@("dirty tracking keys are canonical AbsolutePath, so relative and ./ spellings share one entry")
unittest {
    auto ctx = makeTestContext("dirty_canonical_key");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "a.md", "hello\n");

    import llm.tool_call.utility : pathToWorkarea;

    auto viaRelative = pathToWorkarea(ctx, "a.md").path;
    auto viaExplicit = pathToWorkarea(ctx, "./a.md").path;
    assert(viaRelative == viaExplicit, "AbsolutePath normalizes './a.md'");

    ctx.observe(viaRelative);
    import llm.utility : computeContentHash;

    assert(ctx.observedHash(viaExplicit) == computeContentHash("hello\n"),
            "both spellings share one snapshot entry");
    ctx.markDirty(viaExplicit);
    assert(ctx.isDirty(viaRelative), "both spellings share one dirty entry");
}

@("markDirty flags the path and records the snapshot, observe clears the flag and refreshes it")
unittest {
    auto ctx = makeTestContext("dirty_mark_observe");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\n");

    import llm.tool_call.utility : pathToWorkarea;

    auto p = pathToWorkarea(ctx, "test.txt").path;
    ctx.markDirty(p);
    assert(ctx.isDirty(p), "markDirty sets the flag");
    import llm.utility : computeContentHash;

    assert(ctx.observedHash(p) == computeContentHash("line1\n"), "markDirty records the snapshot");

    ctx.observe(p);
    assert(!ctx.isDirty(p), "observe clears the flag");
    assert(ctx.observedHash(p) == computeContentHash("line1\n"), "observe records the snapshot");
}

@("dirty eviction drops only the oldest clean entries and never evicts dirty entries")
unittest {
    // The tracked state is bounded; eviction drops the OLDEST CLEAN
    // entries only, and dirty entries are never evicted.
    auto ctx = makeTestContext("dirty_eviction");
    scope (exit)
        ctx.teardown;

    import llm.tool_call.utility : pathToWorkarea;
    import llm.utility : computeContentHash;

    AbsolutePath[] cleanPaths;
    AbsolutePath[] dirtyPaths;

    foreach (i; 0 .. 250) {
        auto name = "clean" ~ i.to!string ~ ".txt";
        createTestFile(ctx, name, "y\n");
        auto p = pathToWorkarea(ctx, name).path;
        ctx.observe(p);
        cleanPaths ~= p;
    }
    foreach (i; 0 .. 10) {
        auto name = "dirty" ~ i.to!string ~ ".txt";
        createTestFile(ctx, name, "x\n");
        auto p = pathToWorkarea(ctx, name).path;
        ctx.markDirty(p);
        dirtyPaths ~= p;
    }
    foreach (i; 0 .. 10) {
        auto name = "late" ~ i.to!string ~ ".txt";
        createTestFile(ctx, name, "y\n");
        auto p = pathToWorkarea(ctx, name).path;
        ctx.observe(p);
        cleanPaths ~= p;
    }

    foreach (i; 0 .. 20)
        assert(ctx.observedHash(cleanPaths[i]) == -1, i.to!string ~ " oldest clean entries evicted");
    foreach (i; 20 .. 260)
        assert(ctx.observedHash(cleanPaths[i]) == computeContentHash("y\n"),
                i.to!string ~ " got " ~ ctx.observedHash(cleanPaths[i])
                    .to!string ~ " want " ~ computeContentHash("y\n")
                    .to!string ~ " newer clean entries kept");
    foreach (p; dirtyPaths) {
        assert(ctx.isDirty(p), "dirty entries are never evicted");
        assert(ctx.observedHash(p) == computeContentHash("x\n"), "dirty snapshot intact");
    }
}

@("integration: removeFile on a directory without recursive fails with the directory error")
unittest {
    import llm.tool_call.io.functions : RemoveFileParams, removeFile;

    auto ctx = makeTestContext("remove_dir_no_recursive");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "nested/inner/file.txt", "x\n");

    auto result = removeFile(ctx, RemoveFileParams(path: "nested"));
    assert(!result.success);
    assert(result.msg == "error: path is a directory; remove_file deletes files only — pass recursive=true to delete the directory tree",
            result.msg);
}

@("integration: removeFile with recursive removes a nested directory tree")
unittest {
    import llm.tool_call.io.functions : RemoveFileParams, removeFile;
    import std.file : exists;

    auto ctx = makeTestContext("remove_dir_recursive");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "nested/inner/file.txt", "x\n");
    auto dir = (ctx.workArea ~ "nested").AbsolutePath;
    assert(dir.exists);

    auto result = removeFile(ctx, RemoveFileParams(path: "nested", recursive: true));
    assert(result.success, result.msg);
    assert(result.msg == "OK", result.msg);
    assert(!dir.exists, "tree gone");
}

@("integration: removeFile with recursive=true on a regular file still deletes the file")
unittest {
    import llm.tool_call.io.functions : RemoveFileParams, removeFile;
    import std.file : exists;

    auto ctx = makeTestContext("remove_file_recursive");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "x\n");
    auto f = (ctx.workArea ~ "test.txt").AbsolutePath;

    auto result = removeFile(ctx, RemoveFileParams(path: "test.txt", recursive: true));
    assert(result.success, result.msg);
    assert(!f.exists, "file deleted either way");
}

@("integration: removeFile on a regular file deletes it (no regression)")
unittest {
    import llm.tool_call.io.functions : RemoveFileParams, removeFile;
    import std.file : exists;

    auto ctx = makeTestContext("remove_file_plain");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "x\n");
    auto f = (ctx.workArea ~ "test.txt").AbsolutePath;

    auto result = removeFile(ctx, RemoveFileParams(path: "test.txt"));
    assert(result.success, result.msg);
    assert(result.msg == "OK", result.msg);
    assert(!f.exists, "file deleted");
}

@("integration: removeFile nonexistent-path error unchanged")
unittest {
    import llm.tool_call.io.functions : RemoveFileParams, removeFile;

    auto ctx = makeTestContext("remove_nonexistent");
    scope (exit)
        ctx.teardown;

    auto result = removeFile(ctx, RemoveFileParams(path: "missing.txt"));
    assert(!result.success);
    assert(result.msg.canFind("missing.txt") && result.msg.canFind("do not exist"), result.msg);
}

@(
        "integration: removeFile with recursive unlinks a symlink inside the tree and the outside target survives")
unittest {
    import llm.tool_call.io.functions : RemoveFileParams, removeFile;
    import std.file : exists, mkdirRecurse, rmdirRecurse, symlink, write;

    auto ctx = makeTestContext("remove_tree_inner_symlink");
    scope (exit)
        ctx.teardown;

    // target the tree's symlink points at; not under workArea, so only the
    // test itself may clean it up
    auto outsideDir = ctx.workArea.toString ~ "_outside";
    mkdirRecurse(outsideDir);
    scope (exit)
        rmdirRecurse(outsideDir);
    write(outsideDir ~ "/canary.txt", "keep");

    createTestFile(ctx, "nested/inner.txt", "x\n");
    symlink(outsideDir, (ctx.workArea ~ "nested/link").toString);

    auto result = removeFile(ctx, RemoveFileParams(path: "nested", recursive: true));
    assert(result.success, result.msg);
    assert(!exists((ctx.workArea ~ "nested").toString), "tree gone");
    assert(exists(outsideDir ~ "/canary.txt"), "rmdirRecurse must not follow the symlink");
}

@("integration: removeFile on a symlink path fails and the outside target survives")
unittest {
    import llm.tool_call.io.functions : RemoveFileParams, removeFile;
    import std.file : exists, mkdirRecurse, rmdirRecurse, symlink, write;

    auto ctx = makeTestContext("remove_symlink_path");
    scope (exit)
        ctx.teardown;

    auto outsideDir = ctx.workArea.toString ~ "_outside";
    mkdirRecurse(outsideDir);
    scope (exit)
        rmdirRecurse(outsideDir);
    write(outsideDir ~ "/canary.txt", "keep");
    symlink(outsideDir ~ "/canary.txt", (ctx.workArea ~ "link.txt").toString);

    auto result = removeFile(ctx, RemoveFileParams(path: "link.txt"));
    assert(!result.success);
    assert(result.msg.canFind("symlink"), result.msg);
    assert(exists(outsideDir ~ "/canary.txt"), "target file must survive");
    assert(exists((ctx.workArea ~ "link.txt").toString), "the link itself stays");
}

@(
        "integration: removeFile through a symlinked directory component is rejected and the outside target survives")
unittest {
    import llm.tool_call.io.functions : RemoveFileParams, removeFile;
    import std.file : exists, mkdirRecurse, rmdirRecurse, symlink, write;

    auto ctx = makeTestContext("remove_symlink_component");
    scope (exit)
        ctx.teardown;

    auto outsideDir = ctx.workArea.toString ~ "_outside";
    mkdirRecurse(outsideDir);
    scope (exit)
        rmdirRecurse(outsideDir);
    write(outsideDir ~ "/canary.txt", "keep");
    write(outsideDir ~ "/real.txt", "x\n"); // the file visible through the link
    symlink(outsideDir, (ctx.workArea ~ "dlink").toString);

    auto result = removeFile(ctx, RemoveFileParams(path: "dlink/real.txt"));
    assert(!result.success);
    assert(result.msg.canFind("symlink"), result.msg);
    assert(exists(outsideDir ~ "/real.txt"), "the outside target must survive");
    assert(exists(outsideDir ~ "/canary.txt"), "untouched sibling must survive");
    assert(exists((ctx.workArea ~ "dlink").toString), "the symlink itself stays");
}

@("pathToWorkarea rejects a dangling symlinked directory component with the clean error, and accepts a path whose parent directories do not exist yet")
unittest {
    import llm.tool_call.utility : pathToWorkarea;
    import std.file : exists, rmdirRecurse, symlink;

    auto ctx = makeTestContext("pathtoworkarea_dangling_and_new_dirs");
    scope (exit)
        ctx.teardown;

    // A dangling symlinked component (link exists, target does not) must get
    // the clean PathCheckResult error, not an isSymlink FileException.
    symlink(ctx.workArea.toString ~ "_no_such_dir", (ctx.workArea ~ "dlink").toString);
    auto rejected = pathToWorkarea(ctx, "dlink/real.txt");
    assert(!rejected.valid);
    assert(rejected.errorMsg.canFind("symlink"), rejected.errorMsg);

    // A path whose parent directories do not exist yet is accepted - writeFile
    // relies on this and mkdirRecurse creates them after the guard.
    auto fresh = pathToWorkarea(ctx, "newdir/sub/file.txt");
    assert(fresh.valid, fresh.errorMsg);
    assert(!exists((ctx.workArea ~ "newdir").toString), "pre-condition: parents absent");
}

@(
        "end-to-end: a successful edit marks dirty, a stale byLine fails with re-read guidance, readFile clears it")
unittest {
    import llm.tool_call.io.functions : ReadFileParams, readFile;

    auto ctx = makeTestContext("dirty_stale_byline");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nline2\nline3\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "X", mode: "replace", marker: "line1"));
    assert(result.success, result.msg);

    auto stale = editFile(ctx, UnifiedEditFileParams(path: "test.txt", content: "Y",
            mode: "replace", startLine: 1, count: 1));
    assert(!stale.success, stale.msg);
    assert(stale.msg.canFind("line numbers may be stale"), stale.msg);

    auto read = readFile(ctx, ReadFileParams(path: "test.txt", startLine: 1, count: 5));
    assert(read.success, read.msg);

    auto retry = editFile(ctx, UnifiedEditFileParams(path: "test.txt", content: "Y",
            mode: "replace", startLine: 1, count: 1));
    assert(retry.success, retry.msg);
}

@("end-to-end: a dryRun does not mark dirty, so the following byLine succeeds")
unittest {
    auto ctx = makeTestContext("dirty_dryrun_no_mark");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\n");

    auto dry = editFile(ctx, UnifiedEditFileParams(path: "test.txt", content: "X",
            mode: "replace", marker: "line1", dryRun: true));
    assert(dry.success, dry.msg);

    auto after = editFile(ctx, UnifiedEditFileParams(path: "test.txt", content: "X",
            mode: "replace", startLine: 1, count: 1));
    assert(after.success, after.msg);
}

@("end-to-end: a failed edit does not mark dirty, so the following byLine succeeds")
unittest {
    auto ctx = makeTestContext("dirty_failed_no_mark");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\n");

    auto failed = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "X", mode: "replace", marker: "nope"));
    assert(!failed.success, failed.msg);

    auto after = editFile(ctx, UnifiedEditFileParams(path: "test.txt", content: "X",
            mode: "replace", startLine: 1, count: 1));
    assert(after.success, after.msg);
}

@(
        "end-to-end: an external write is caught by the snapshot cross-check, readFile refresh lets the retry succeed")
unittest {
    import std.file : write;

    import llm.tool_call.io.functions : ReadFileParams, readFile;
    import llm.tool_call.utility : pathToWorkarea;

    auto ctx = makeTestContext("dirty_external_write");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\nline2\n");

    // Record the snapshot without going through the tools (as readFile does).
    auto p = pathToWorkarea(ctx, "test.txt").path;
    ctx.observe(p);

    // External (non-context) write: the content changes, so the hash
    // cross-check catches it.
    write(p.toString, "line1\nline2\nexternal\n");

    auto stale = editFile(ctx, UnifiedEditFileParams(path: "test.txt", content: "X",
            mode: "replace", startLine: 1, count: 1));
    assert(!stale.success, stale.msg);
    assert(stale.msg.canFind("line numbers may be stale"), stale.msg);

    auto read = readFile(ctx, ReadFileParams(path: "test.txt", startLine: 1, count: 5));
    assert(read.success, read.msg);

    auto retry = editFile(ctx, UnifiedEditFileParams(path: "test.txt", content: "X",
            mode: "replace", startLine: 1, count: 1));
    assert(retry.success, retry.msg);
}

@(
        "a file removed outside the tools is not flagged stale, the existence check reports the real problem")
unittest {
    import std.file : remove;

    import llm.tool_call.utility : pathToWorkarea;

    auto ctx = makeTestContext("dirty_removed_external");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "line1\n");
    auto p = pathToWorkarea(ctx, "test.txt").path;
    ctx.observe(p);

    remove(p.toString);

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "X", mode: "replace", startLine: 1, count: 1));
    assert(!result.success, result.msg);
    assert(!result.msg.canFind("stale"), result.msg);
}

@("classifyEditError maps the stale byLine target message to stale_byline_target")
unittest {
    assert(classifyEditError("Path 'a.md' was modified by a successful edit earlier in this session; line numbers may be stale — re-read it with readFile, then retry with fresh startLine/count, or switch to byMarker/byContent targeting.") == "stale_byline_target");
}

@(
        "integration: editFile echoes replaced lines elided past 11 (11 lines render untruncated), identically under dryRun")
unittest {
    auto ctx = makeTestContext("editreplaced");
    scope (exit)
        ctx.teardown;

    string content;
    for (int i = 1; i <= 12; i++)
        content ~= "old" ~ i.to!string ~ "\n";
    content ~= "tail\n";
    createTestFile(ctx, "test.txt", content);

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt", content: "new1\nnew2\n",
            mode: "replace", startLine: 1, count: 12, dryRun: true));
    assert(result.success, result.msg);

    auto json = parseJSON(result.msg);
    assert(json["replacedTruncated"].boolean, json.toString);
    assert(json["replacedTotal"].integer == 12, json.toString);
    auto rep = json["replaced"].array;
    assert(rep.length == 11, json.toString); // first 5 + marker + last 5
    assert(rep[0].str == "old1", json.toString);
    assert(rep[4].str == "old5", json.toString);
    assert(rep[5].str == "… (12 lines replaced; first 5 and last 5 shown)", json.toString);
    assert(rep[10].str == "old12", json.toString);
}

@(
        "integration: editFile reports noOp for an identical-content replace under dryRun, file untouched")
unittest {
    // The no-op detection compares the composed preview against the original
    // lines in memory, so it behaves identically with dryRun=true (nothing is
    // written either way).
    auto ctx = makeTestContext("editnoop");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "a\nb\nc\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "b\n", mode: "replace", startLine: 2, count: 1, dryRun: true));
    assert(result.success, result.msg);

    auto json = parseJSON(result.msg);
    assert(json["ok"].boolean, json.toString);
    assert(json["noOp"].boolean, json.toString);
    assert(json["note"].str.canFind("no change"), json.toString);
    assert(json["linesChanged"].integer == 0, json.toString);
    assert(readTestFile(ctx, "test.txt") == "a\nb\nc\n", "dryRun never writes");
}

@("integration: editFile combines the noOp note with the autoCount note when both apply")
unittest {
    // byContent replace with count omitted whose content equals the matched
    // block: autoCountUsed=true AND noOp=true. The shared `note` slot must
    // carry BOTH messages (auto-count derivation first, no-op explanation
    // appended), never clobber one with the other.
    auto ctx = makeTestContext("editnoopnote");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "head\nfoo\nbar\ntail\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "foo\nbar\n", mode: "replace", searchContent: "foo\nbar"));
    assert(result.success, result.msg);

    auto json = parseJSON(result.msg);
    assert(json["ok"].boolean, json.toString);
    assert(json["noOp"].boolean, json.toString);
    assert(json["autoCountUsed"].boolean, json.toString);
    const note = json["note"].str;
    assert(note.canFind("auto-derived"), note);
    assert(note.canFind("no change"), note);
}

@(
        "integration: editFile warns on insert-mode anchor duplication (byMarker insert_after), file shows the duplication")
unittest {
    auto ctx = makeTestContext("editanchordup");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "m\nb\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "m\nX", mode: "insert_after", marker: "m"));
    assert(result.success, result.msg);

    auto json = parseJSON(result.msg);
    assert(json["ok"].boolean, json.toString);
    assert(json["anchorDuplicate"].boolean, json.toString);
    const note = json["note"].str;
    assert(note.canFind("appears twice"), note);
    assert(note.canFind("duplicates"), note);
    {
        import std.array : replace;

        const actual = readTestFile(ctx, "test.txt");
        assert(actual == "m\nm\nX\nb\n", actual.replace("\n", "<NL>"));
    }
}

@("integration: editFile warns on insert-mode anchor duplication under dryRun without writing")
unittest {
    auto ctx = makeTestContext("editanchordupdry");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "m\nb\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "X\nm", mode: "insert_before", marker: "m", dryRun: true));
    assert(result.success, result.msg);

    auto json = parseJSON(result.msg);
    assert(json["ok"].boolean, json.toString);
    assert(json["anchorDuplicate"].boolean, json.toString);
    assert(json["note"].str.canFind("appears twice"), json.toString);
    assert(readTestFile(ctx, "test.txt") == "m\nb\n", "dryRun never writes");
}

@("integration: editFile echoes replaced lines untruncated at 11 or fewer, absent for insert")
unittest {
    auto ctx = makeTestContext("editreplacedsmall");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "a\nb\nc\nd\ne\nf\n");

    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "X\nY\n", mode: "replace", startLine: 3, count: 3));
    assert(result.success, result.msg);

    auto json = parseJSON(result.msg);
    auto rep = json["replaced"].array;
    assert(rep.length == 3, json.toString); // no elision at <= 11 lines
    assert(rep[0].str == "c", json.toString);
    assert(rep[2].str == "e", json.toString);
    assert("replacedTotal" !in json, json.toString); // replacedTotal only with elision
    assert("replacedTruncated" !in json, json.toString); // no elision at <= 11 lines

    // Boundary: exactly 11 replaced lines render untruncated (11 rendered
    // entries are not fewer than the total, so there is nothing to elide)
    createTestFile(ctx, "test11.txt", "l1\nl2\nl3\nl4\nl5\nl6\nl7\nl8\nl9\nl10\nl11\n");
    auto r11 = editFile(ctx, UnifiedEditFileParams(path: "test11.txt", content: "n1\nn2\nn3\nn4\nn5\nn6\nn7\nn8\nn9\nn10\nn11\n",
            mode: "replace", startLine: 1, count: 11));
    assert(r11.success, r11.msg);
    auto j11 = parseJSON(r11.msg);
    auto rep11 = j11["replaced"].array;
    assert(rep11.length == 11, j11.toString);
    assert(rep11[0].str == "l1", j11.toString);
    assert(rep11[10].str == "l11", j11.toString);
    assert("replacedTotal" !in j11, j11.toString);
    assert("replacedTruncated" !in j11, j11.toString);

    // Refresh the stale-byLine snapshot: the replace above marked the file dirty
    readFile(ctx, ReadFileParams(path: "test.txt", startLine: 1, count: 5));

    // Insert modes consume nothing: no replaced field at all
    auto ins = editFile(ctx, UnifiedEditFileParams(path: "test.txt", content: "Z\n",
            mode: "insert_after", startLine: 1));
    assert(ins.success, ins.msg);
    auto insJson = parseJSON(ins.msg);
    assert("replaced" !in insJson, ins.msg);
    assert("replacedTotal" !in insJson, ins.msg);
    assert("replacedTruncated" !in insJson, ins.msg);
}

@(
        "integration: byContent insert with duplication combines the autoCount note with the anchorDuplicate note")
unittest {
    auto ctx = makeTestContext("editanchordupbycontent");
    scope (exit)
        ctx.teardown;

    createTestFile(ctx, "test.txt", "head\nfoo\nbar\ntail\n");

    // byContent insert_after with count omitted: autoCountUsed=true (the count
    // is auto-derived from the matched block) AND anchorDuplicate=true (the
    // content's first line "bar" equals the matched block's last line "bar").
    // The shared `note` slot must carry BOTH messages (auto-count derivation
    // first, anchor-duplication appended), never clobber one with the other.
    auto result = editFile(ctx, UnifiedEditFileParams(path: "test.txt",
            content: "bar\nbaz", mode: "insert_after", searchContent: "foo\nbar"));
    assert(result.success, result.msg);

    auto json = parseJSON(result.msg);
    assert(json["ok"].boolean, json.toString);
    assert(json["autoCountUsed"].boolean, json.toString);
    assert(json["anchorDuplicate"].boolean, json.toString);
    const note = json["note"].str;
    assert(note.canFind("auto-derived"), note);
    assert(note.canFind("appears twice"), note);
}
