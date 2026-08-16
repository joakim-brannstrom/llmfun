/// Unit tests for the session store and reference resolver. Temp-dir based,
/// mirroring the `llm.tool_call.io.tests` pattern.
module llm.session.tests;

version (unittest) {
    import std.algorithm : canFind;
    import std.datetime : Clock;
    import std.file : exists, rmdirRecurse, readText, mkdirRecurse;
    import std.json : JSONValue, JSONType;
    import std.path : buildPath;
    import std.string : format, strip;

    import my.optional : hasValue, orElse;
    import my.path : Path;

    import llm.session : SessionId, SessionStore, SessionMeta, SessionFile, resolveSessionRef;
    import llm.session.types : isValidId, PreviewMaxChars;

    private string makeTempDir(string name) {
        auto now = Clock.currTime();
        auto ts = format("%04d%02d%02d%02d%02d%02d", now.year, now.month,
                now.day, now.hour, now.minute, now.second);
        auto base = buildPath("llmfun_test", "session_" ~ name ~ "_" ~ ts);
        mkdirRecurse(base);
        return base;
    }

    private void cleanupDir(string dir) {
        try {
            rmdirRecurse(dir);
        } catch (Exception) {
            // Silently ignore cleanup failures during test teardown
        }
    }

    private void writeFileContent(string path, string content) {
        import std.stdio : File;

        auto f = File(path, "w");
        f.write(content);
    }
}

// --- Test: create() generates valid id and writes file ---

unittest {
    auto tmpDir = makeTempDir("create");
    scope (exit)
        cleanupDir(tmpDir);

    auto store = new SessionStore(tmpDir.Path);
    auto meta = store.create();

    // Id format: YYYYMMDD-HHMMSS-NNNN
    import std.regex : regex, match;
    import std.range : empty;

    auto pat = regex(r"^\d{8}-\d{6}-[0-9a-f]{4}$");
    assert(!meta.id.get.match(pat).empty, "id format invalid: " ~ meta.id.get);

    auto filePath = buildPath(tmpDir, meta.id.get ~ ".json");
    assert(exists(filePath), "session file not created");

    // Meta fields
    assert(meta.title.length > 0, "title should not be empty");
    assert(meta.createdAt > 0, "createdAt should be set");
    assert(meta.updatedAt == meta.createdAt, "updatedAt should equal createdAt");
    assert(meta.messageCount == 0, "messageCount should be 0");
    assert(meta.userMessageCount == 0, "userMessageCount should be 0");
}

// --- Test: create() id uniqueness ---

unittest {
    auto tmpDir = makeTempDir("create_unique");
    scope (exit)
        cleanupDir(tmpDir);

    auto store = new SessionStore(tmpDir.Path);
    const N = 100;
    string[] ids;
    foreach (i; 0 .. N) {
        auto meta = store.create();
        assert(!canFind(ids, meta.id.get), "duplicate id: " ~ meta.id.get);
        ids ~= meta.id.get;
    }
}

// --- Test: load() round-trip with create ---

unittest {
    auto tmpDir = makeTempDir("load_roundtrip");
    scope (exit)
        cleanupDir(tmpDir);

    auto store = new SessionStore(tmpDir.Path);
    auto meta = store.create();
    auto loadOpt = store.load(meta.id);

    assert(hasValue(loadOpt), "load should succeed for created session");
    auto sf = orElse(loadOpt, SessionFile());
    assert(sf.meta.id == meta.id, "id mismatch");
    assert(sf.meta.title == meta.title, "title mismatch");
    assert(sf.doc.type == JSONType.object, "doc should be an object");
}

// --- Test: load() returns none for missing file ---

unittest {
    auto tmpDir = makeTempDir("load_missing");
    scope (exit)
        cleanupDir(tmpDir);

    auto store = new SessionStore(tmpDir.Path);
    auto loadOpt = store.load(SessionId("nonexistent-id"));
    assert(!hasValue(loadOpt), "load should return none for missing file");
}

// --- Test: save() with messages ---

unittest {
    auto tmpDir = makeTempDir("save_messages");
    scope (exit)
        cleanupDir(tmpDir);

    auto store = new SessionStore(tmpDir.Path);
    auto meta = store.create();

    // Build a doc with messages
    JSONValue doc;
    doc["messages"] = JSONValue();
    doc["messages"].array = [];

    JSONValue userMsg;
    userMsg["role"] = "user";
    userMsg["content"] = "hello world";
    doc["messages"].array ~= userMsg;

    JSONValue assistantMsg;
    assistantMsg["role"] = "assistant";
    assistantMsg["content"] = "hi there";
    doc["messages"].array ~= assistantMsg;

    auto savedMeta = store.save(meta.id, meta, doc);
    assert(savedMeta.messageCount == 2, "messageCount should be 2");
    assert(savedMeta.userMessageCount == 1, "userMessageCount should be 1");
    assert(savedMeta.preview == "hello world", "preview should match user message");
    assert(savedMeta.updatedAt >= savedMeta.createdAt, "updatedAt should be >= createdAt");
}

// --- Test: rename() rejects empty title ---

unittest {
    auto tmpDir = makeTempDir("rename_empty");
    scope (exit)
        cleanupDir(tmpDir);

    auto store = new SessionStore(tmpDir.Path);
    auto meta = store.create();

    auto result = store.rename(meta.id, "");
    assert(!hasValue(result), "rename should reject empty title");
}

// --- Test: rename() preserves updatedAt ---

unittest {
    auto tmpDir = makeTempDir("rename_preserve");
    scope (exit)
        cleanupDir(tmpDir);

    auto store = new SessionStore(tmpDir.Path);
    auto meta = store.create();
    auto originalUpdatedAt = meta.updatedAt;

    auto result = store.rename(meta.id, "New Title");
    assert(hasValue(result), "rename should succeed");
    auto renamedMeta = orElse(result, SessionMeta());
    assert(renamedMeta.title == "New Title", "title should be updated");
    assert(renamedMeta.updatedAt == originalUpdatedAt, "updatedAt should be preserved");
}

// --- Test: rename() fails for missing session ---

unittest {
    auto tmpDir = makeTempDir("rename_missing");
    scope (exit)
        cleanupDir(tmpDir);

    auto store = new SessionStore(tmpDir.Path);
    auto result = store.rename(SessionId("nonexistent-id"), "New Title");
    assert(!hasValue(result), "rename should fail for missing session");
}

// --- Test: remove() works for existing file ---

unittest {
    auto tmpDir = makeTempDir("remove_existing");
    scope (exit)
        cleanupDir(tmpDir);

    auto store = new SessionStore(tmpDir.Path);
    auto meta = store.create();
    auto filePath = buildPath(tmpDir, meta.id.get ~ ".json");
    assert(exists(filePath), "file should exist before remove");

    store.remove(meta.id);
    assert(!exists(filePath), "file should be removed");
}

// --- Test: remove() is no-op for missing file (valid-format and invalid ids) ---

unittest {
    auto tmpDir = makeTempDir("remove_missing");
    scope (exit)
        cleanupDir(tmpDir);

    auto store = new SessionStore(tmpDir.Path);
    store.remove(SessionId("nonexistent-id")); // D12-invalid: no-op
    store.remove(SessionId("20250101-120000-abcd")); // valid format, file absent: silent no-op
}

// --- Test: list() returns sorted by updatedAt desc ---

unittest {
    auto tmpDir = makeTempDir("list_sorted");
    scope (exit)
        cleanupDir(tmpDir);

    auto store = new SessionStore(tmpDir.Path);
    auto m1 = store.create();

    auto m2 = store.create();
    // m2 should have updatedAt >= m1.updatedAt (same second or later)

    auto sessions = store.list();
    assert(sessions.length == 2, "should list 2 sessions");
    // Most recent (or equal) should be first
    assert(sessions[0].updatedAt >= sessions[1].updatedAt, "should be sorted descending");
}

// --- Test: list() skips corrupt files ---

unittest {
    auto tmpDir = makeTempDir("list_corrupt");
    scope (exit)
        cleanupDir(tmpDir);

    auto store = new SessionStore(tmpDir.Path);
    auto meta = store.create();

    // Create a corrupt file
    writeFileContent(buildPath(tmpDir, "20250101-120000-abcd.json"), "not valid json");

    auto sessions = store.list();
    assert(sessions.length == 1, "should skip corrupt file");
    assert(sessions[0].id == meta.id, "should only list valid session");
}

// --- Test: D12 ID validation rejects invalid formats ---

unittest {
    assert(isValidId(SessionId("20250101-120000-abcd")), "valid id should pass");
    assert(isValidId(SessionId("20250101-120000-0000")), "zero suffix should pass");
    assert(isValidId(SessionId("20250101-120000-ffff")), "all f suffix should pass");

    // Invalid ids
    assert(!isValidId(SessionId("")), "empty string should fail");
    assert(!isValidId(SessionId("20250101-120000-abcde")), "5-char suffix should fail");
    assert(!isValidId(SessionId("20250101-120000-abc")), "3-char suffix should fail");
    assert(!isValidId(SessionId("../etc/passwd")), "path traversal should fail");
    assert(!isValidId(SessionId("not-a-valid-id")), "non-matching format should fail");
    assert(!isValidId(SessionId("20250101-120000-ABCD")), "uppercase hex should fail");
}

// --- Test: save() rejects invalid D12 id ---

unittest {
    auto tmpDir = makeTempDir("save_invalid_id");
    scope (exit)
        cleanupDir(tmpDir);

    auto store = new SessionStore(tmpDir.Path);
    auto meta = store.create();

    JSONValue doc;
    doc["messages"] = JSONValue();
    doc["messages"].array = [];

    auto savedMeta = store.save(SessionId("invalid-id"), meta, doc);
    assert(savedMeta.id == meta.id, "meta should be unchanged for invalid id");
}

// --- Test: preview truncation to PreviewMaxChars ---

unittest {
    auto tmpDir = makeTempDir("preview_truncate");
    scope (exit)
        cleanupDir(tmpDir);

    auto store = new SessionStore(tmpDir.Path);
    auto meta = store.create();

    // Build a doc with a long user message
    JSONValue doc;
    doc["messages"] = JSONValue();
    doc["messages"].array = [];

    JSONValue userMsg;
    userMsg["role"] = "user";
    userMsg["content"] = "This is a very long message that exceeds the preview limit";
    doc["messages"].array ~= userMsg;

    auto savedMeta = store.save(meta.id, meta, doc);
    assert(savedMeta.preview.length <= PreviewMaxChars,
            "preview should be truncated to PreviewMaxChars");
    assert(savedMeta.preview == "This is a very long messa",
            "preview should be exactly truncated: got '" ~ savedMeta.preview ~ "'");
}

// --- Test: preview with no user messages ---

unittest {
    auto tmpDir = makeTempDir("preview_no_user");
    scope (exit)
        cleanupDir(tmpDir);

    auto store = new SessionStore(tmpDir.Path);
    auto meta = store.create();

    JSONValue doc;
    doc["messages"] = JSONValue();
    doc["messages"].array = [];

    JSONValue assistantMsg;
    assistantMsg["role"] = "assistant";
    assistantMsg["content"] = "hello";
    doc["messages"].array ~= assistantMsg;

    auto savedMeta = store.save(meta.id, meta, doc);
    assert(savedMeta.preview == "", "preview should be empty with no user messages");
}

// --- Test: preview skips non-string content ---

unittest {
    auto tmpDir = makeTempDir("preview_nonstring");
    scope (exit)
        cleanupDir(tmpDir);

    auto store = new SessionStore(tmpDir.Path);
    auto meta = store.create();

    JSONValue doc;
    doc["messages"] = JSONValue();
    doc["messages"].array = [];

    JSONValue userMsg1;
    userMsg1["role"] = "user";
    userMsg1["content"] = JSONValue(); // Array content, not string
    userMsg1["content"].array = [JSONValue("item1"), JSONValue("item2")];
    doc["messages"].array ~= userMsg1;

    JSONValue userMsg2;
    userMsg2["role"] = "user";
    userMsg2["content"] = "actual text message";
    doc["messages"].array ~= userMsg2;

    auto savedMeta = store.save(meta.id, meta, doc);
    assert(savedMeta.preview == "actual text message",
            "preview should skip non-string content and find next user message");
}

// --- Test: preview truncation is grapheme-safe (A17) ---

unittest {
    auto tmpDir = makeTempDir("preview_grapheme");
    scope (exit)
        cleanupDir(tmpDir);

    auto store = new SessionStore(tmpDir.Path);
    auto meta = store.create();

    // 24 ASCII chars + a 2-byte char at the boundary: a raw byte slice
    // would split the multi-byte char; grapheme truncation keeps it whole.
    auto twentyFourAscii = "aaaaaaaaaaaaaaaaaaaaaaa" ~ "a";

    JSONValue doc;
    doc["messages"] = JSONValue();
    doc["messages"].array = [];

    JSONValue userMsg;
    userMsg["role"] = "user";
    userMsg["content"] = twentyFourAscii ~ "\u00E9" ~ " rest of message";
    doc["messages"].array ~= userMsg;

    auto savedMeta = store.save(meta.id, meta, doc);
    import std.uni : byGrapheme;
    import std.algorithm : count;

    assert(savedMeta.preview.byGrapheme.count == PreviewMaxChars,
            "preview should hold exactly PreviewMaxChars graphemes");
    assert(savedMeta.preview == twentyFourAscii ~ "\u00E9",
            "multi-byte grapheme at the boundary must stay intact: got '" ~ savedMeta.preview ~ "'");

    // e + combining acute accent is ONE grapheme; it must never be split.
    JSONValue doc2;
    doc2["messages"] = JSONValue();
    doc2["messages"].array = [];

    JSONValue userMsg2;
    userMsg2["role"] = "user";
    userMsg2["content"] = twentyFourAscii ~ "e\u0301" ~ "x";
    doc2["messages"].array ~= userMsg2;

    auto savedMeta2 = store.save(meta.id, savedMeta, doc2);
    assert(savedMeta2.preview == twentyFourAscii ~ "e\u0301",
            "combining grapheme must stay whole: got '" ~ savedMeta2.preview ~ "'");
}

// --- Test: preview normalizes newlines/tabs/control bytes to spaces (A17) ---

unittest {
    auto tmpDir = makeTempDir("preview_single_line");
    scope (exit)
        cleanupDir(tmpDir);

    auto store = new SessionStore(tmpDir.Path);
    auto meta = store.create();

    JSONValue doc;
    doc["messages"] = JSONValue();
    doc["messages"].array = [];

    JSONValue userMsg;
    userMsg["role"] = "user";
    userMsg["content"] = "line1\nline2\ttabbed\r\nend";
    doc["messages"].array ~= userMsg;

    auto savedMeta = store.save(meta.id, meta, doc);
    auto preview = savedMeta.preview;

    assert(!canFind(preview, "\n") && !canFind(preview, "\r") && !canFind(preview,
            "\t"), "preview must be a single line: got '" ~ preview ~ "'");
    assert(preview == "line1 line2 tabbed  end",
            "control bytes should become spaces: got '" ~ preview ~ "'");
}

// --- Test: sweepEmptySessions removes empty non-kept sessions only ---

unittest {
    auto tmpDir = makeTempDir("sweep_empty");
    scope (exit)
        cleanupDir(tmpDir);

    auto store = new SessionStore(tmpDir.Path);

    // Active session stays empty on purpose: keep must exempt it (W15).
    auto active = store.create();
    auto emptyNonActive = store.create();

    auto nonEmpty = store.create();
    JSONValue doc;
    doc["messages"] = JSONValue();
    doc["messages"].array = [];
    JSONValue userMsg;
    userMsg["role"] = "user";
    userMsg["content"] = "hello";
    doc["messages"].array ~= userMsg;
    auto savedMeta = store.save(nonEmpty.id, nonEmpty, doc);
    assert(savedMeta.userMessageCount == 1, "setup: non-empty session should have 1 user message");

    auto removed = store.sweepEmptySessions(active.id);

    assert(removed.length == 1 && removed[0] == emptyNonActive.id,
            "only the empty non-active session should be swept");
    assert(!exists(buildPath(tmpDir, emptyNonActive.id.get ~ ".json")),
            "empty non-active file should be gone");
    assert(exists(buildPath(tmpDir, active.id.get ~ ".json")),
            "active session must survive even when empty");
    assert(exists(buildPath(tmpDir, nonEmpty.id.get ~ ".json")), "non-empty session must survive");
}

// --- Test: sweepEmptySessions never touches corrupt files ---

unittest {
    auto tmpDir = makeTempDir("sweep_corrupt");
    scope (exit)
        cleanupDir(tmpDir);

    auto store = new SessionStore(tmpDir.Path);
    auto empty1 = store.create();
    auto empty2 = store.create();

    writeFileContent(buildPath(tmpDir, "20250101-120000-abcd.json"), "not valid json");

    // keep id not in the store: all empty sessions are swept
    auto removed = store.sweepEmptySessions(SessionId("20250101-120000-9999"));
    assert(removed.length == 2, "both empty sessions should be swept");
    assert(exists(buildPath(tmpDir, "20250101-120000-abcd.json")),
            "corrupt file must never be swept");
}

// --- Test: sweepEmptySessions on an empty store ---

unittest {
    auto tmpDir = makeTempDir("sweep_empty_store");
    scope (exit)
        cleanupDir(tmpDir);

    auto store = new SessionStore(tmpDir.Path);
    auto removed = store.sweepEmptySessions(SessionId("20250101-120000-9999"));
    assert(removed.length == 0, "no sessions to sweep");
}

// --- Test: extra key preservation round-trip ---

unittest {
    auto tmpDir = makeTempDir("extra_keys");
    scope (exit)
        cleanupDir(tmpDir);

    auto store = new SessionStore(tmpDir.Path);
    auto meta = store.create();

    // Manually add extra key to the file
    auto filePath = buildPath(tmpDir, meta.id.get ~ ".json");
    auto content = readText(filePath);
    // Insert extra key before closing brace
    content = content.strip;
    content = content[0 .. $ - 1] ~ ",\"customKey\":\"customValue\"}";
    writeFileContent(filePath, content);

    // Load and verify extra key is preserved
    auto loadOpt = store.load(meta.id);
    assert(hasValue(loadOpt), "load should succeed");
    auto sf = orElse(loadOpt, SessionFile());
    assert(sf.meta.extra.type == JSONType.object, "extra should be an object");
    assert(sf.meta.extra["customKey"].str == "customValue", "extra key should be preserved");

    // Save and verify extra key survives round-trip
    JSONValue doc;
    doc["messages"] = JSONValue();
    doc["messages"].array = [];
    auto savedMeta = store.save(meta.id, sf.meta, doc);

    auto loadOpt2 = store.load(meta.id);
    assert(hasValue(loadOpt2), "load should succeed after save");
    auto sf2 = orElse(loadOpt2, SessionFile());
    assert(sf2.meta.extra["customKey"].str == "customValue", "extra key should survive save");
}

// --- Test: .tmp sweep removes stale files ---

unittest {
    auto tmpDir = makeTempDir("tmp_sweep");
    scope (exit)
        cleanupDir(tmpDir);

    // Pre-create a stale .tmp file
    writeFileContent(buildPath(tmpDir, "20250101-120000-abcd.json.tmp"), "{}");

    // Constructor should sweep it
    auto store = new SessionStore(tmpDir.Path);
    assert(!exists(buildPath(tmpDir, "20250101-120000-abcd.json.tmp")),
            "stale .tmp file should be removed");
}

// --- Test: constructor throws on unusable directory ---

unittest {
    // Use a path that should not be writable (e.g., under /proc)
    auto badPath = "/proc/nonexistent_session_dir/chat".Path;
    bool threw = false;
    try {
        auto store = new SessionStore(badPath);
    } catch (Exception) {
        threw = true;
    }
    assert(threw, "constructor should throw for unwritable directory");
}

// --- Test: resolveSessionRef index precedence ---

unittest {
    SessionMeta s1, s2, s3;
    s1.id = SessionId("id1");
    s1.title = "First";
    s2.id = SessionId("id2");
    s2.title = "Second";
    s3.id = SessionId("id3");
    s3.title = "Third";
    auto sessions = [s1, s2, s3];

    // Index 1 should return first session
    auto result = resolveSessionRef(sessions, "1");
    assert(hasValue(result), "index 1 should resolve");
    assert(orElse(result, SessionId.init) == SessionId("id1"),
            "index 1 should return first session");

    // Index 3 should return third session
    result = resolveSessionRef(sessions, "3");
    assert(hasValue(result), "index 3 should resolve");
    assert(orElse(result, SessionId.init) == SessionId("id3"),
            "index 3 should return third session");
}

// --- Test: resolveSessionRef id precedence over title ---

unittest {
    SessionMeta s1, s2;
    s1.id = SessionId("myid");
    s1.title = "My Title";
    s2.id = SessionId("otherid");
    s2.title = "myid"; // title matches s1's id
    auto sessions = [s1, s2];

    // "myid" should match s1 by id, not s2 by title
    auto result = resolveSessionRef(sessions, "myid");
    assert(hasValue(result), "should resolve");
    assert(orElse(result, SessionId.init) == SessionId("myid"), "id match should take precedence");
}

// --- Test: resolveSessionRef case-insensitive title match ---

unittest {
    SessionMeta s1;
    s1.id = SessionId("id1");
    s1.title = "Hello World";
    auto sessions = [s1];

    auto result = resolveSessionRef(sessions, "hello world");
    assert(hasValue(result), "case-insensitive title should match");
    assert(orElse(result, SessionId.init) == SessionId("id1"), "should return correct id");
}

// --- Test: resolveSessionRef returns none for no match ---

unittest {
    SessionMeta s1;
    s1.id = SessionId("id1");
    s1.title = "Title";
    auto sessions = [s1];

    auto result = resolveSessionRef(sessions, "nonexistent");
    assert(!hasValue(result), "should return none for no match");
}

// --- Test: resolveSessionRef empty sessions array ---

unittest {
    SessionMeta[] sessions;
    auto result = resolveSessionRef(sessions, "1");
    assert(!hasValue(result), "should return none for empty array");
}

// --- Test: resolveSessionRef out of range index ---

unittest {
    SessionMeta s1;
    s1.id = SessionId("id1");
    s1.title = "Title";
    auto sessions = [s1];

    auto result = resolveSessionRef(sessions, "5");
    assert(!hasValue(result), "out of range index should return none");
}
