/// `SessionStore`: persistent storage for chat sessions, one JSON file per
/// session under a directory. The store keeps no in-memory cache (D4); all
/// operations read from or write to the filesystem.
module llm.session.store;

import logger = std.logger;
import std.algorithm : canFind, min, sort;
import std.conv : text;
import std.datetime : Clock;
import std.file : dirEntries, exists, readText, stdRemove = remove,
    stdRename = rename, SpanMode, mkdirRecurse;
import std.json : JSONValue, parseJSON, JSONType, JSONOptions;
import std.path : buildPath, baseName;
import std.stdio : File;
import std.string : strip;

import my.optional : Optional, none, some, hasValue, orElse;
import my.path : AbsolutePath, Path;

import llm.session.types : KnownHeaderKeys, PreviewMaxChars, SessionFile,
    SessionId, SessionMeta, generateDateTitle, generateId, isValidId;
import llm.utility : getValue;

/** Log a warning safely: the nested catch keeps `@safe nothrow` logging
 * from aborting the surrounding operation (AGENTS.md error-handling rule). */
private void safeWarn(Args...)(string fmt, Args args) @safe nothrow {
    try {
        logger.warningf(fmt, args);
    } catch (Exception) {
    }
}

/** Persistent store for chat session files.
 *
 * The store keeps no in-memory cache (D4); all operations read from or write
 * to the filesystem. Each session is one JSON file under `chatDir`.
 *
 * Thread safety: single writer (the agent thread); no locking needed.
 */
class SessionStore {
    private AbsolutePath chatDir;

    /** Create a session store for the given directory.
     *
     * Creates the directory if it does not exist. Sweeps stale `.tmp` files
     * left by previous crashes (W14). The directory is resolved to an
     * absolute path so all operations stay independent of the CWD.
     *
     * Params:
     *   chatDir = path to the chat sessions directory
     *
     * Throws: `Exception` if the directory cannot be created or is unusable
     */
    this(Path chatDir) {
        this.chatDir = chatDir.AbsolutePath;

        if (!this.chatDir.exists) {
            mkdirRecurse(chatDir.toString);
        }

        // Verify the directory is usable
        if (!this.chatDir.exists) {
            throw new Exception(i"Cannot create session directory: $(chatDir)".text);
        }

        sweepStaleTmpFiles();
    }

    /** Absolute path of the session file for an id (no D12 check - callers
     * validate). */
    private AbsolutePath filePathFor(SessionId id) @trusted {
        return AbsolutePath(buildPath(chatDir.toString, id.get ~ ".json"));
    }

    /** Sweep stale `.tmp` files from the chat directory (W14). */
    private void sweepStaleTmpFiles() {
        foreach (entry; dirEntries(chatDir, "*.json.tmp", SpanMode.shallow)) {
            auto name = entry.name.baseName;
            try {
                stdRemove(entry.name);
                logger.tracef("Removed stale tmp file: %s", name);
            } catch (Exception e) {
                safeWarn("Failed to remove stale tmp file '%s': %s", name, e.msg);
            }
        }
    }

    /** Get the messages array from a JSON document, defaulting to empty array. */
    private JSONValue getMessagesArray(JSONValue doc) @trusted {
        if (doc.type == JSONType.object && "messages" in doc.object) {
            auto msgs = doc["messages"];
            if (msgs.type == JSONType.array) {
                return msgs;
            }
        }
        // Return an explicit empty array JSONValue
        JSONValue emptyArr;
        emptyArr.array = [];
        return emptyArr;
    }

    /** Compute messageCount, userMessageCount, and preview from messages array.
     *
     * Preview short-circuits at the first string-content user message (W8).
     * Counts require the full scan.
     */
    private void computeCountsAndPreview(JSONValue doc, ref SessionMeta meta) @trusted {
        size_t msgCount = 0;
        size_t userMsgCount = 0;
        string previewStr;

        auto msgs = getMessagesArray(doc);
        foreach (entry; msgs.array) {
            msgCount++;

            if (entry.type == JSONType.object) {
                auto role = getValue!(string)(entry, v => v["role"].str, "");
                if (role == "user") {
                    userMsgCount++;
                    // Extract preview from first user message with string content
                    if (previewStr.length == 0) {
                        auto content = getValue!(string)(entry, v => v["content"].str, "");
                        if (content.length > 0) {
                            previewStr = content[0 .. min(content.length, PreviewMaxChars)];
                        }
                    }
                }
            }
        }

        meta.messageCount = msgCount;
        meta.userMessageCount = userMsgCount;
        meta.preview = previewStr;
    }

    /** Parse the header fields and compute counts/preview from a JSON document.
     *
     * Extracts known header keys (title, createdAt, updatedAt), preserves
     * unknown keys in `extra` (D2), and computes messageCount/
     * userMessageCount/preview from the messages array.
     */
    private SessionMeta parseHeader(SessionId sessionId, JSONValue doc) @trusted {
        auto meta = SessionMeta();
        meta.id = sessionId;

        meta.title = getValue!(string)(doc, v => v["title"].str, "");
        if (meta.title.strip.length == 0) {
            meta.title = generateDateTitle();
        }
        meta.createdAt = getValue!long(doc, v => v["createdAt"].integer, 0L);
        meta.updatedAt = getValue!long(doc, v => v["updatedAt"].integer, meta.createdAt);

        computeCountsAndPreview(doc, meta);

        if (doc.type == JSONType.object) {
            JSONValue extraObj;
            foreach (key, val; doc.object) {
                if (!canFind(KnownHeaderKeys, key)) {
                    extraObj[key] = val;
                }
            }
            if (extraObj.type == JSONType.object && extraObj.object.length > 0) {
                meta.extra = extraObj;
            }
        }

        return meta;
    }

    /** Read and parse a session file, returning none on missing/corrupt file. */
    private Optional!JSONValue readFile(SessionId id) @trusted {
        auto filePath = filePathFor(id);
        try {
            if (!exists(filePath)) {
                return none!JSONValue();
            }
            auto content = readText(filePath);
            auto doc = parseJSON(content);
            return some(doc);
        } catch (Exception e) {
            safeWarn("Corrupt session file '%s': %s", id.get, e.msg);
            return none!JSONValue();
        }
    }

    /** Atomic write: write to tmp file then rename (same pattern as Agent.saveHistory). */
    private void atomicWrite(SessionId id, string content) @trusted {
        auto filePath = filePathFor(id);
        auto tmpPath = filePath.toString ~ ".tmp";
        {
            auto f = File(tmpPath, "w");
            f.write(content);
        }
        stdRename(tmpPath, filePath.toString);
    }

    /** Build the session file document: meta.extra + header + messages. */
    private JSONValue buildSessionDoc(SessionMeta meta, long updatedAt, JSONValue messages) @trusted {
        JSONValue outDoc;

        if (meta.extra.type == JSONType.object) {
            foreach (key, val; meta.extra.object) {
                outDoc[key] = val;
            }
        }

        outDoc["title"] = meta.title;
        outDoc["createdAt"] = meta.createdAt;
        outDoc["updatedAt"] = updatedAt;
        outDoc["messages"] = messages;
        return outDoc;
    }

    /** Serialize a session document to the canonical pretty-printed form. */
    private static string serializeDoc(JSONValue doc) @safe {
        return doc.toPrettyString(JSONOptions.doNotEscapeSlashes);
    }

    /** Create a new session with a generated id and date title.
     *
     * Generates an id per D10, sets title to local date, creates with
     * createdAt == updatedAt == now, and writes the file immediately with
     * messages: [].
     *
     * Returns: the metadata of the newly created session
     */
    SessionMeta create() @trusted {
        auto now = Clock.currTime().toUnixTime();

        auto id = generateId((SessionId candidate) {
            return exists(filePathFor(candidate));
        });

        auto meta = SessionMeta();
        meta.id = id;
        meta.title = generateDateTitle();
        meta.createdAt = now;
        meta.updatedAt = now;

        JSONValue messages;
        messages.array = [];
        auto doc = buildSessionDoc(meta, now, messages);
        atomicWrite(id, serializeDoc(doc));

        return meta;
    }

    /** Load a session by id.
     *
     * Returns none if id fails D12 validation, file is missing, or corrupt.
     */
    Optional!SessionFile load(SessionId id) @trusted {
        if (!isValidId(id)) {
            safeWarn("Invalid session id format: '%s'", id.get);
            return none!SessionFile();
        }

        auto docOpt = readFile(id);
        if (!hasValue(docOpt)) {
            return none!SessionFile();
        }

        auto doc = orElse(docOpt, JSONValue());
        auto meta = parseHeader(id, doc);

        auto sf = SessionFile();
        sf.meta = meta;
        sf.doc = doc;
        return some(sf);
    }

    /** Save a session, bumping updatedAt and recomputing counts/preview.
     *
     * `doc` contributes only `messages[]`; the header is rebuilt from
     * `meta.extra` + title/createdAt + updatedAt: now. Returns the updated
     * meta with recomputed counts/preview. If `doc["messages"]` is missing,
     * treats it as []. D12-invalid id returns meta unchanged with a warning.
     */
    SessionMeta save(SessionId id, SessionMeta meta, JSONValue doc) @trusted {
        if (!isValidId(id)) {
            safeWarn("Invalid session id format, skipping save: '%s'", id.get);
            return meta;
        }

        auto now = Clock.currTime().toUnixTime();

        // Copy messages from doc (M5: only doc["messages"])
        auto outDoc = buildSessionDoc(meta, now, getMessagesArray(doc));

        // Recompute counts/preview
        meta = parseHeader(id, outDoc);
        meta.updatedAt = now;

        atomicWrite(id, serializeDoc(outDoc));

        return meta;
    }

    /** Remove a session file by id.
     *
     * No-op (silent) if the file does not exist. D12-invalid id is ignored
     * with a warning.
     */
    void remove(SessionId id) @trusted {
        if (!isValidId(id)) {
            safeWarn("Invalid session id format for remove: '%s'", id.get);
            return;
        }
        auto filePath = filePathFor(id);
        if (!exists(filePath)) {
            return;
        }
        try {
            stdRemove(filePath);
        } catch (Exception e) {
            safeWarn("Failed to remove session '%s': %s", id.get, e.msg);
        }
    }

    /** Rename a session title.
     *
     * Updates the header title only, resaves, preserves updatedAt (D11).
     * Returns none on empty title, unknown id, or D12-invalid id.
     */
    Optional!SessionMeta rename(SessionId id, string newTitle) @trusted {
        if (newTitle.strip.length == 0) {
            safeWarn("Rename rejected: empty title");
            return none!SessionMeta();
        }
        if (!isValidId(id)) {
            safeWarn("Invalid session id format for rename: '%s'", id.get);
            return none!SessionMeta();
        }

        auto loadOpt = load(id);
        if (!hasValue(loadOpt)) {
            safeWarn("Rename failed: session '%s' not found", id.get);
            return none!SessionMeta();
        }

        auto sf = orElse(loadOpt, SessionFile());
        sf.meta.title = newTitle.strip;

        auto outDoc = buildSessionDoc(sf.meta, sf.meta.updatedAt, getMessagesArray(sf.doc));
        try {
            atomicWrite(id, serializeDoc(outDoc));
        } catch (Exception e) {
            safeWarn("Rename save failed for '%s': %s", id.get, e.msg);
            return none!SessionMeta();
        }

        return some(sf.meta);
    }

    /** List all sessions, sorted by updatedAt descending (ties: id descending).
     *
     * Scans *.json files in chatDir, parses header keys and computes
     * messageCount/userMessageCount/preview from the messages array.
     * Corrupt files are skipped with a warning, never thrown (N2/N3).
     */
    SessionMeta[] list() @trusted {
        SessionMeta[] result;

        foreach (entry; dirEntries(chatDir, "*.json", SpanMode.shallow)) {
            auto fileName = baseName(entry.name); // e.g. "20250101-120000-abcd.json"

            // Guard against filenames too short to contain ".json" extension
            if (fileName.length < 6) {
                safeWarn("Skipping file with suspiciously short name: '%s'", fileName);
                continue;
            }
            auto idStr = fileName[0 .. $ - 5]; // strip ".json"
            auto id = SessionId(idStr);

            if (!isValidId(id)) {
                safeWarn("Skipping file with invalid id: '%s'", fileName);
                continue;
            }

            auto docOpt = readFile(id);
            if (!hasValue(docOpt)) {
                continue; // already warned in readFile
            }

            auto doc = orElse(docOpt, JSONValue());
            auto meta = parseHeader(id, doc);
            result ~= meta;
        }

        // Sort by updatedAt descending; break ties by id descending so the
        // order (and /sessions numbering) is deterministic across runs.
        result.sort!((a, b) => a.updatedAt > b.updatedAt || (a.updatedAt == b.updatedAt
                && a.id > b.id));

        return result;
    }
}
