/**
Turn-stamped chat history.

Every history entry carries a typed `turnId` (0 = no turn) mixed in via
`TurnIdMixin`. A turn is one user query plus everything produced while
answering it. The stamping policy lives in `Chat.add`: a user-query Message
opens a new turn (allocating from the per-Chat counter `nextTurnId_`), every
other insertion continues the current turn, and `setSystemPrompt` stamps 0
(the system prompt belongs to no turn).

Invariants:
- I1: history is sorted by (turn_id, position); insertion, load
  reconstruction, and compression preserve it. setHistory is a raw
  replacement - the caller owns the ordering.
- I2: every non-system message added while a turn is active has turn_id > 0
  (temporary chats may stay turn 0).
- I3: currentTurnId_ <= nextTurnId_ always; the per-session counter never
  decreases.
- I4: TurnIDs are strictly increasing across user queries and never re-used
  within a session across process lifetimes; cross-session identity is the
  composite (session_id, turn_id).

TurnID lifecycle: allocate (add/beginNewTurn), stamp (add), persist
(toSaveJson writes save_data["turn_id"]; the session header next_turn_id
holds the counter high-water mark), load (fromJson/fromUser extract the
stamp; reconstructTurnIds heals legacy and partially stamped files), clear
(resets the current turn, never the counter), compress (the merged summary
carries the summarized slice's turnEnd; kept X/Y messages keep their
stamps). The Facts/Trace projections getDialogueHistory/getReasoningTrace
expose the dialogue and reasoning views that Phases 1-2 index.
 */
module llm.chat;

import logger = std.logger;
import std.algorithm : filter, map, sum, min, canFind;
import std.array : array, replace, appender;
import std.conv : to, text;
import std.exception : collectException;
import std.json : JSONValue, JSONOptions, parseJSON, JSONType;
import std.range : enumerate, isOutputRange, empty;
import std.sumtype : SumType, match;
import std.typecons : Tuple, tuple;
import std.utf : byUTF, toUTF8, validate, UTFException;
import llm.common.config : ApproxTokenSize;
import llm.utility : getValue;

struct Chat {
    alias MessageT = SumType!(Message, ToolMessage, ToolResponse, VisionMessage);

    private {
        MessageT[] history;
        size_t prevIndex;
        long nextTurnId_; // counter high-water mark; persisted as session header next_turn_id
        long currentTurnId_; // turn stamped on insertion (0 = no active turn)
    }

    // Allocates the next TurnID (A2). Per-instance state only: each Chat is owned
    // by one thread and each session is one file with one Chat, so no
    // static/shared/atomic synchronization is needed (N1). Internal: external
    // turn starts go through beginNewTurn()/add() (A3).
    private long allocateTurnId() @safe pure nothrow @nogc {
        return ++nextTurnId_;
    }

    // Explicitly opens a new turn for non-Message turn starts (e.g. vision chats).
    long beginNewTurn() @safe pure nothrow @nogc {
        currentTurnId_ = allocateTurnId();
        return currentTurnId_;
    }

    long currentTurnId() @safe pure nothrow const @nogc {
        return currentTurnId_;
    }

    long nextTurnId() @safe pure nothrow const @nogc {
        return nextTurnId_;
    }

    void setSystemPrompt(string x) {
        auto m = Message(Role.system, userQuery: false, content: x, thinking: null);
        m.turnId = 0; // the system prompt belongs to no turn (A3)
        if (history.empty)
            history ~= MessageT(m);
        else
            history[0] = m;
    }

    void clear() @safe pure nothrow @nogc {
        currentTurnId_ = 0; // no-reuse: the next user query gets a strictly larger ID
        if (history.empty)
            return;
        history = history[0 .. 1];
        prevIndex = 1;
    }

    // A user query always opens a new turn (A3); call sites cannot violate it.
    void add(Message m) @safe pure nothrow {
        if (m.isUserQuery) {
            currentTurnId_ = allocateTurnId();
        }
        m.turnId = currentTurnId_;
        history ~= MessageT(m);
    }

    void add(ToolMessage m) @safe pure nothrow {
        m.turnId = currentTurnId_;
        history ~= MessageT(m);
    }

    void add(ToolResponse m) @safe pure nothrow {
        m.turnId = currentTurnId_;
        history ~= MessageT(m);
    }

    void add(VisionMessage m) @safe pure nothrow {
        m.turnId = currentTurnId_;
        history ~= MessageT(m);
    }

    /// Adds a user query as a Message: opens a new turn (A3).
    void addUserQuery(string query) @safe nothrow {
        add(Message(Role.user, userQuery: true, content: query, thinking: null));
    }

    void resetResponseIndex() @safe nothrow {
        prevIndex = history.length;
    }

    MessageT[] lastResponse() @safe nothrow {
        if (history.empty)
            return null;
        return history[history.length - 1 .. $];
    }

    MessageT[] lastResponses() @safe nothrow {
        // nothing has happend in the history
        if (history.length <= 1 || prevIndex == history.length)
            return null;
        // after a compression
        if (prevIndex >= history.length)
            prevIndex = 1;
        return history[prevIndex .. $];
    }

    MessageT[] getMessages() @safe nothrow {
        return history;
    }

    Message[] getUserQueries() @safe nothrow {
        auto app = appender!(Message[])();

        foreach (msg; history) {
            msg.match!((Message m) {
                if (m.role == Role.user && m.isUserQuery) {
                    app.put(m);
                }
            }, (_) {});
        }
        return app[];
    }

    // A4 Facts projection: dialogue entries in canonical order — user queries
    // (user role with isUserQuery), assistant final text Messages (non-empty
    // content), and ToolMessages carrying taskDoneAnswer (isFinalAnswer).
    // Harness control traffic (userQuery:false nudges) is excluded (H1).
    // VisionMessage is not classified by A4 and appears in neither projection.
    // Entries are returned whole, keeping their typed TurnIDs. Note:
    // isUserQuery() checks save_data["user"] key presence, not its value, so a
    // hand-edited {"user": false} entry would still classify as a user query.
    MessageT[] getDialogueHistory() @safe nothrow const {
        MessageT[] result;

        foreach (msg; history) {
            const bool isDialogue = msg.match!((Message m) {
                return (m.role == Role.user && m.isUserQuery)
                        || (m.role == Role.assistant && !m.content.empty);
            }, (ToolMessage m) {
                return m.isFinalAnswer();
            }, (_) {
                return false;
            });
            if (isDialogue) {
                result ~= msg;
            }
        }
        return result;
    }

    // A4 Trace projection: reasoning entries in canonical order — non-final
    // ToolMessages (tool calls), every ToolResponse (tool outputs), and all
    // thinking strings (Message.thinking). Membership is not exclusive: an
    // assistant final Message carrying both content and thinking appears in
    // both projections (H1) — entries are returned whole and cannot be split.
    // H1 is categorical here: harness control traffic (user-role Messages with
    // userQuery == false) is excluded even if it carried thinking. ToolMessage
    // membership is exclusive: a final (taskDone) ToolMessage's thinking is
    // not carried — revisit if Phase 2 needs the pre-taskDone reasoning.
    MessageT[] getReasoningTrace() @safe nothrow const {
        MessageT[] result;

        foreach (msg; history) {
            const bool isTrace = msg.match!((Message m) {
                if (m.role == Role.user && !m.isUserQuery)
                    return false; // H1: harness control traffic is neither projection
                return !m.thinking.empty;
            }, (ToolMessage m) {
                return !m.isFinalAnswer();
            }, (ToolResponse m) {
                return true;
            }, (_) {
                return false;
            });
            if (isTrace) {
                result ~= msg;
            }
        }
        return result;
    }

    long approxContextSize() @safe nothrow {
        return history.map!(a => approxMessageSize(a)).sum;
    }

    // Raw history replacement - compression's only path to rewrite history.
    // I1 contract: the caller must pass an array sorted by (turn_id, position)
    // and must not reorder inside a turn; setHistory neither sorts nor stamps.
    // compress() satisfies this: its merged summary carries the summarized
    // slice's turnEnd (C2) and sits before the kept X/Y tail whose ids are
    // >= turnEnd. prevIndex is left untouched; after a shrink lastResponses()
    // re-anchors it to 1 when it exceeds the new length.
    void setHistory(MessageT[] x) @safe nothrow {
        history = x;
    }

    void load(JSONValue json) @trusted nothrow {
        try {
            if (history.empty) {
                setSystemPrompt(null);
            }

            const startLen = history.length;
            foreach (entry; json["messages"].array) {
                const role = entry["role"].str.to!Role;

                final switch (role) {
                case Role.system:
                    break;
                case Role.assistant:
                    if ("tool_calls" in entry) {
                        ToolMessage m;
                        m.fromJson(entry);
                        history ~= MessageT(m);
                    } else {
                        Message m;
                        m.fromJson(entry);
                        history ~= MessageT(m);
                    }
                    break;
                case Role.tool: {
                        ToolResponse m;
                        m.fromJson(entry);
                        history ~= MessageT(m);
                    }
                    break;
                case Role.user:
                    history ~= fromUser(entry);
                    break;
                }
            }
            reconstructTurnIds(history[startLen .. $],
                    getValue!long(json, (v) => v["next_turn_id"].integer, 0L));

            logger.tracef("Loaded previous chat history. Size %s->%s", startLen, history.length);
        } catch (Exception e) {
            logger.trace(e).collectException;
            logger.trace(e.msg).collectException;
        }
    }

    /// Replaces invalid UTF-8 sequences in all messages with U+FFFD (Unicode
    /// replacement character). Never discards a message; valid content is left
    /// untouched (no copy).
    /// JSONValue fields (metadata, saveData, toolCalls) are intentionally NOT
    /// sanitized here; they are covered by body-level sanitization when the
    /// history is serialized for a request.
    /// Returns: number of messages that were modified.
    size_t sanitizeHistory() @safe {
        size_t modified;
        foreach (ref msg; history) {
            bool changed = false;
            msg.match!(
                (ref Message a) {
                    changed = sanitizeField(a.content);
                    if (sanitizeField(a.thinking))
                        changed = true;
                },
                (ref ToolMessage a) {
                    changed = sanitizeField(a.thinking);
                },
                (ref ToolResponse a) {
                    changed = sanitizeField(a.content);
                    if (sanitizeField(a.toolCallId))
                        changed = true;
                    if (sanitizeField(a.toolName))
                        changed = true;
                },
                (ref VisionMessage a) {
                    changed = sanitizeField(a.content);
                    if (sanitizeField(a.imageDataUrl))
                        changed = true;
                }
            );
            if (changed)
                modified++;
        }
        return modified;
    }

    // A5 reconstruction for loaded entries: seed the counter from the header
    // high-water mark, then fill missing stamps in file order. Entries that
    // already carry a turn_id are never re-stamped (H2).
    private void reconstructTurnIds(MessageT[] entries, long headerNextTurnId)
            @safe nothrow {
        long maxStamped = 0;
        foreach (entry; entries) {
            const id = turnIdOf(entry);
            if (id > maxStamped)
                maxStamped = id;
        }

        // Prefix allocation: the stored value is the last allocated ID (I-1,
        // no +1). The max with the current value keeps I3 (never decreases).
        if (headerNextTurnId > nextTurnId_)
            nextTurnId_ = headerNextTurnId;
        if (maxStamped > nextTurnId_)
            nextTurnId_ = maxStamped;

        if (entries.length == 0) {
            // load() is expected to run on a cleared chat (activateSession
            // clears first), so a messages-less file legitimately resets the
            // current turn.
            currentTurnId_ = 0;
            return;
        }

        if (maxStamped > 0) {
            // Mixed file: a gap inherits the previous entry's ID. An unstamped
            // prefix takes the file's first stamped ID, so the walk stays
            // monotonic (I1) and no counter ID is burned (H2). This shape is
            // live today: /compact persists an unstamped merged summary before
            // the stamped X/Y tail (summary_agent newHistory) and the next
            // commit writes it back.
            long firstStamped = 0;
            foreach (entry; entries) {
                const id = turnIdOf(entry);
                if (id != 0) {
                    firstStamped = id;
                    break;
                }
            }

            long prevId = 0;
            foreach (ref entry; entries) {
                const id = turnIdOf(entry);
                if (id != 0) {
                    prevId = id;
                } else if (prevId != 0) {
                    stampEntry(entry, prevId);
                } else {
                    // maxStamped > 0 guarantees firstStamped != 0.
                    prevId = firstStamped;
                    stampEntry(entry, prevId);
                }
            }
        } else {
            // Pure legacy file: a user query opens a new turn; everything
            // after it continues that turn until the next query. Messages
            // before the first query stay 0.
            long current = 0;
            foreach (ref entry; entries) {
                if (isUserQueryEntry(entry))
                    current = allocateTurnId();
                stampEntry(entry, current);
            }
        }

        long maxId = 0;
        foreach (entry; entries) {
            const id = turnIdOf(entry);
            if (id > maxId)
                maxId = id;
        }
        currentTurnId_ = maxId;
    }

    JSONValue toJson() @safe {
        JSONValue root;
        root["messages"] = history.map!(a => a.match!((a) => a.toJson)).array;
        return root;
    }

    JSONValue toSaveJson() @safe {
        JSONValue root;
        root["messages"] = history.map!(a => a.match!((a) => a.toSaveJson)).array;
        return root;
    }

    size_t length() @safe pure nothrow const @nogc {
        return history.length;
    }

    string toString() @safe const {
        auto buf = appender!string;
        toString(buf);
        return buf.data;
    }

    void toString(Writer)(ref Writer w) const if (isOutputRange!(Writer, char)) {
        import std.format : formattedWrite;
        import std.range : put;

        put(w, "Chat(");
        foreach (a; history) {
            a.match!((Message a) => formattedWrite(w, `[%s, "%s"]`, a.role,
                    a.content), (ToolMessage a) => formattedWrite(w, `[%s, "%s"]`, a.role,
                    a.toolCalls.toString(JSONOptions.doNotEscapeSlashes)),
                    (ToolResponse a) => formattedWrite(w, `[%s, \"%s\", id:%s, name:%s]`,
                        a.role, a.content, a.toolCallId, a.toolName),
                    (VisionMessage a) => formattedWrite(w, `[%s, "%s", image]`, "user", a.content));
        }
        put(w, ")");
    }
}

/// Returns: the approximate tokens a message consist of including the thinking part.
long approxMessageSize(T)(T msg) @safe nothrow {
    try {
        // dfmt off
        return msg.match!(
        (Message a) {
            return (a.content.length + a.thinking.length) / ApproxTokenSize;
        }, (ToolMessage a) {
            return (a.toolCalls.toString(JSONOptions.doNotEscapeSlashes).length + a.thinking.length) / ApproxTokenSize;
        }, (ToolResponse a) {
            return a.content.length / ApproxTokenSize;
        }, (VisionMessage a) {
            return (a.content.length + a.imageDataUrl.length) / ApproxTokenSize;
        });
        // dfmt on
    } catch (Exception e) {
    }
    return 0;
}

// Typed TurnID on every message type: 0 = unstamped (system prompt, temporary
// chat padding). In memory the field is the single source of truth; on disk it
// is persisted inside save_data["turn_id"] (A5, Task 3).
mixin template TurnIdMixin() {
    long turnId = 0;
}

// Reads a message's typed TurnID field. 0 = no turn. Pure field read, no JSON.
long turnIdOf(Chat.MessageT msg) @safe pure nothrow @nogc {
    return msg.match!(
        (Message m) => m.turnId,
        (ToolMessage m) => m.turnId,
        (ToolResponse m) => m.turnId,
        (VisionMessage m) => m.turnId);
}

// (min, max) TurnID over a slice. Phase 1 uses this directly as chunk metadata
// {turn_start, turn_end}. An empty slice yields (0, 0). Unstamped entries
// (turnId == 0) participate in the range: a slice containing any of them yields
// turnStart == 0. Phase 1 consumers must filter turnId != 0 or treat 0 as
// "before the first stamped turn".
Tuple!(long, "turnStart", long, "turnEnd") turnRangeOf(const(Chat.MessageT)[] msgs)
        @safe pure nothrow @nogc {
    if (msgs.length == 0)
        return tuple!("turnStart", "turnEnd")(0L, 0L);
    long lo = long.max;
    long hi = long.min;
    foreach (m; msgs) {
        const id = turnIdOf(m);
        if (id < lo)
            lo = id;
        if (id > hi)
            hi = id;
    }
    return tuple!("turnStart", "turnEnd")(lo, hi);
}

// Writes the typed field onto the message. Plain field store — the stamp
// cannot fail (N3).
private void stampEntry(ref Chat.MessageT entry, long id) @safe nothrow {
    entry.match!(
        (ref Message m) => m.turnId = id,
        (ref ToolMessage m) => m.turnId = id,
        (ref ToolResponse m) => m.turnId = id,
        (ref VisionMessage m) => m.turnId = id);
}

// True only for user-query Messages whose save_data["user"] is the JSON
// boolean true (A5.5). The value check (not just key presence) keeps a legacy
// {"user": false} from opening a spurious turn.
private bool isUserQueryEntry(const Chat.MessageT entry) @safe nothrow {
    bool result = false;
    entry.match!((Message m) {
        result = getValue!bool(m.saveData, (v) => v["user"].boolean, false);
    }, (_) {});
    return result;
}

// Reads save_data["turn_id"] into the typed field (A5). 0 when absent/corrupt.
private long turnIdFromSaveData(JSONValue saveData) @trusted {
    return getValue!long(saveData, (v) => v["turn_id"].integer, 0L);
}

// Rebuilds saveData as a fresh object carrying turn_id (A5): the in-memory
// saveData AA is never mutated (JSONValue copies share the underlying AA, so
// the object is copied entry by entry). Non-object saveData is returned
// as-is and never hosts turn_id (never produced today).
private JSONValue saveDataWithTurnId(JSONValue saveData, long turnId) @trusted {
    if (saveData.type != JSONType.object && saveData.type != JSONType.null_) {
        return saveData;
    }
    JSONValue outData;
    if (saveData.type == JSONType.object) {
        foreach (key, val; saveData.object) {
            outData[key] = val;
        }
    }
    if (turnId != 0) {
        outData["turn_id"] = turnId;
    }
    return outData;
}

struct VisionMessage {
    mixin TurnIdMixin;
    string content;
    string imageDataUrl;
    JSONValue metadata;

    this(string content, string imageDataUrl, JSONValue metadata = JSONValue.init) @safe nothrow {
        this.content = content;
        this.imageDataUrl = imageDataUrl;
        this.metadata = metadata;
    }

    JSONValue toJson() @safe {
        auto contentArr = [
            JSONValue(["type": JSONValue("text"), "text": JSONValue(content)]),
            JSONValue([
                "type": JSONValue("image_url"),
                "image_url": JSONValue.emptyObject
            ])
        ];
        contentArr[1]["image_url"] = JSONValue(["url": JSONValue(imageDataUrl)]);

        auto root = JSONValue();
        root["role"] = JSONValue("user");
        root["content"] = contentArr;

        return root;
    }

    JSONValue toSaveJson() @safe {
        auto j = toJson();
        if (metadata != JSONValue.init) {
            j["metadata"] = metadata;
        }
        if (turnId != 0) {
            JSONValue sd;
            sd["turn_id"] = turnId;
            j["save_data"] = sd;
        }
        return j;
    }

    void fromJson(JSONValue entry) @trusted nothrow {
        try {
            string text;
            string imageDataUrl;
            foreach (item; entry["content"].array) {
                if (item["type"].str == "text") {
                    text = item["text"].str;
                } else if (item["type"].str == "image_url") {
                    if (item["image_url"].type == JSONType.object) {
                        imageDataUrl = item["image_url"]["url"].str;
                    } else {
                        imageDataUrl = item["image_url"].str;
                    }
                }
            }

            if (auto s = "save_data" in entry) {
                this.turnId = turnIdFromSaveData(*s);
            }
        } catch (Exception e) {
            logger.trace(e.msg).collectException;
        }
    }

    string toString() @safe const {
        string imgPreview;
        if (imageDataUrl.length > 60) {
            imgPreview = "[" ~ imageDataUrl[0 .. 57] ~ "...]";
        } else {
            imgPreview = "[" ~ imageDataUrl ~ "]";
        }
        return i"VisionMessage(content:$(content) image:$(imgPreview))".text;
    }
}

struct Message {
@safe:
    mixin TurnIdMixin;
    Role role;
    string content;
    string thinking;
    JSONValue metadata; // for external tools only
    JSONValue saveData; // for llmfun internal use

    this(Role role, bool userQuery, string content, string thinking,
            JSONValue metaData = JSONValue.init, JSONValue saveData = JSONValue.init) @safe nothrow {
        this.role = role;
        this.content = content;
        this.thinking = thinking;
        this.metadata = metaData;
        this.saveData = saveData;

        try {
            if (userQuery) {
                this.saveData["user"] = true;
            }
        } catch (Exception e) {
        }
    }

    size_t length() @safe const nothrow {
        return content.length;
    }

    string toString() @safe const {
        return i"Message(role:$(role) user:$(isUserQuery) content:$(content) thinking:$(
                thinking[0 .. min(thinking.length, 80)]) saveData:$(saveData))".text;
    }

    bool isUserQuery() @safe pure const nothrow {
        if (saveData.type != JSONType.object)
            return false;
        try {
            return ("user" in saveData) !is null;
        } catch (Exception e) {
        }
        return false;
    }

    // toJson — REST API: does NOT include thinking (no change needed)
    JSONValue toJson() @safe {
        auto j = JSONValue(["role": role.to!string, "content": content]);
        j["reasoning_content"] = thinking;
        return j;
    }

    JSONValue toSaveJson() @safe {
        auto j = toJson();
        if (metadata != JSONValue.init) {
            j["metadata"] = metadata;
        }
        if (saveData != JSONValue.init || turnId != 0) {
            j["save_data"] = saveDataWithTurnId(saveData, turnId);
        }
        return j;
    }

    void fromJson(JSONValue j) @trusted nothrow {
        this.thinking = null;
        try {
            this.role = j["role"].str.to!Role;
            this.content = j["content"].str;
            if (auto r = "reasoning_content" in j) {
                this.thinking = r.str;
            }
            if (auto m = "metadata" in j) {
                this.metadata = *m;
            }
            if (auto s = "save_data" in j) {
                this.saveData = *s;
                this.turnId = turnIdFromSaveData(this.saveData);
            }
        } catch (Exception e) {
            logger.trace(e.msg).collectException;
        }
    }
}

// Convert a JSON object to VisionMessage or Message
Chat.MessageT fromUser(JSONValue entry) {
    Chat.MessageT rval;
    if (entry["content"].type == JSONType.array) {
        // Multi-modal content (VisionMessage)
        string text;
        string imageDataUrl;
        foreach (item; entry["content"].array) {
            if (item["type"].str == "text") {
                text = item["text"].str;
            } else if (item["type"].str == "image_url") {
                if (item["image_url"].type == JSONType.object) {
                    imageDataUrl = item["image_url"]["url"].str;
                } else {
                    imageDataUrl = item["image_url"].str;
                }
            }
        }
        if (imageDataUrl) {
            auto metadata = getValue(entry, (v) => v["metadata"], JSONValue.init);
            auto saveData = getValue(entry, (v) => v["save_data"], JSONValue.init);
            auto vm = VisionMessage(text, imageDataUrl, metadata);
            vm.turnId = turnIdFromSaveData(saveData); // M1: load() routes all user entries here
            rval = vm;
        } else {
            string thinking = getValue(entry, (v) => v["reasoning_content"].str, null);
            auto metadata = getValue(entry, (v) => v["metadata"], JSONValue.init);
            auto saveData = getValue(entry, (v) => v["save_data"], JSONValue.init);
            auto m = Message(Role.user, userQuery: false, content: text,
                    thinking: thinking, metaData: metadata, saveData: saveData);
            m.turnId = turnIdFromSaveData(saveData);
            rval = m;
        }
    } else {
        Message m;
        m.fromJson(entry);
        rval = m;
    }
    return rval;
}

struct ToolMessage {
@safe:
    mixin TurnIdMixin;
    Role role;
    string thinking;
    JSONValue toolCalls;
    JSONValue metadata; // for external tools only
    JSONValue saveData; // for llmfun internal use

    this(string thinking, JSONValue toolCalls, JSONValue metadata = JSONValue.init,
            JSONValue saveData = JSONValue.init) @safe nothrow {
        this.thinking = thinking;
        this.role = Role.assistant;
        this.toolCalls = toolCalls;
        this.metadata = metadata;
        this.saveData = saveData;
    }

    size_t length() @safe const {
        return toolCalls.toString(JSONOptions.doNotEscapeSlashes).length;
    }

    string toString() @safe const {
        return i"Message(role:$(role) toolCalls:$(
                toolCalls.toString(JSONOptions.doNotEscapeSlashes)) saveData:$(
                saveData.toString(JSONOptions.doNotEscapeSlashes)))".text;
    }

    JSONValue toJson() @safe {
        return JSONValue([
            "role": JSONValue(role.to!string),
            "content": JSONValue(null),
            "tool_calls": toolCalls,
            "reasoning_content": JSONValue(thinking)
        ]);
    }

    JSONValue toSaveJson() @safe {
        auto j = toJson();
        if (metadata != JSONValue.init) {
            j["metadata"] = metadata;
        }
        if (saveData != JSONValue.init || turnId != 0) {
            j["save_data"] = saveDataWithTurnId(saveData, turnId);
        }
        return j;
    }

    void fromJson(JSONValue j) @trusted nothrow {
        try {
            this.role = j["role"].str.to!Role;
            this.toolCalls = j["tool_calls"];
            if (auto r = "reasoning_content" in j) {
                this.thinking = r.str;
            }
            if (auto m = "metadata" in j) {
                this.metadata = *m;
            }
            if (auto s = "save_data" in j) {
                this.saveData = *s;
                this.turnId = turnIdFromSaveData(this.saveData);
            }
        } catch (Exception e) {
            logger.trace(e.msg).collectException;
        }
    }

    /// Returns true if this ToolMessage represents a final answer.
    bool isFinalAnswer() const @safe nothrow {
        if (saveData.type != JSONType.object)
            return false;
        try {
            return ("taskDoneAnswer" in saveData) !is null;
        } catch (Exception e) {
        }
        return false;
    }

    /// Returns: the final answer text or empty string.
    string getFinalAnswer() const @safe {
        return getValue(saveData, (v) => v["taskDoneAnswer"].str, null);
    }

    /// Returns all tool call names and call IDs in this message
    Tuple!(string, "name", string, "callId")[] getFunctions() @system {
        if (toolCalls.type != JSONType.array)
            return [];
        auto arr = toolCalls.array;
        Tuple!(string, "name", string, "callId")[] result;
        result.length = arr.length;
        size_t idx = 0;
        foreach (call; arr) {
            auto name = "";
            auto callId = "";
            if ("function" in call && "name" in call["function"])
                name = call["function"]["name"].str;
            if ("id" in call)
                callId = call["id"].str;
            result[idx++] = tuple!("name", "callId")(name, callId);
        }
        return result;
    }

    /// Removes all tool calls matching the given name. Returns count removed.
    size_t removeTool(string toolName) @system {
        if (toolCalls.type != JSONType.array)
            return 0;

        const len = toolCalls.array.length;
        auto tools = toolCalls.array.filter!(a => getValue(a,
                (a) => a["function"]["name"].str, null) != toolName).array;
        toolCalls = JSONValue(tools);
        return len - tools.length;
    }

    /// Returns true if any tool call matches the given name.
    bool hasTool(string toolName) @system const {
        if (toolCalls.type != JSONType.array)
            return false;
        foreach (call; toolCalls.array
                .map!(a => getValue(a, (a) => a["function"]["name"].str, null))
                .filter!(a => a == toolName)) {
            return true;
        }
        return false;
    }
}

struct ToolResponse {
@safe:
    mixin TurnIdMixin;
    Role role;
    string content;
    string toolCallId;
    string toolName;
    JSONValue metadata; // for external tools only
    JSONValue saveData; // for llmfun internal use

    this(string content, string toolCallId, string toolName, bool success,
            JSONValue metadata = JSONValue.init, JSONValue saveData = JSONValue.init) @safe nothrow {
        this.role = Role.tool;
        this.content = content;
        this.toolCallId = toolCallId;
        this.toolName = toolName;
        this.metadata = metadata;
        this.saveData = saveData;
        try {
            this.saveData["success"] = success;
        } catch (Exception e) {
            try {
                logger.tracef("ToolResponse: failed to store success in saveData: %s", e.msg);
            } catch (Exception) {
            }
        }
    }

    size_t length() @safe const nothrow {
        return content.length;
    }

    string toString() @safe const {
        return i"Message(role:$(role) toolName:$(toolName) content:$(content))".text;
    }

    /// if the tool succeeded or not.
    bool success() @safe {
        return getValue(saveData, (v) => v["success"].boolean, false);
    }

    JSONValue toJson() @safe {
        return JSONValue([
            "role": role.to!string,
            "content": content,
            "tool_call_id": toolCallId,
            "name": toolName
        ]);
    }

    JSONValue toSaveJson() @safe {
        auto j = toJson();
        if (metadata != JSONValue.init) {
            j["metadata"] = metadata;
        }
        if (saveData != JSONValue.init || turnId != 0) {
            j["save_data"] = saveDataWithTurnId(saveData, turnId);
        }
        return j;
    }

    void fromJson(JSONValue j) @trusted nothrow {
        try {
            this.role = j["role"].str.to!Role;
            this.content = j["content"].str;
            this.toolCallId = j["tool_call_id"].str;
            this.toolName = j["name"].str;
            if (auto m = "metadata" in j) {
                this.metadata = *m;
            }
            if (auto s = "save_data" in j) {
                this.saveData = *s;
                this.turnId = turnIdFromSaveData(this.saveData);
            }
        } catch (Exception e) {
            logger.trace(e.msg).collectException;
        }
    }

    /// Returns tool name and call ID as a Tuple
    Tuple!(string, "name", string, "callId") getFunction() @safe const {
        return tuple!("name", "callId")(toolName, toolCallId);
    }
}

enum Role {
    user,
    assistant,
    system,
    tool
}

// Check if a ToolMessage should be hidden from user output
bool isHiddenToolCall(JSONValue toolCalls) {
    if (toolCalls.type != JSONType.array || toolCalls.array.empty)
        return false;
    foreach (call; toolCalls.array) {
        if ("function" !in call)
            continue;
        auto name = call["function"]["name"].str;
        if (hiddenToolNames.canFind(name))
            return true;
    }
    return false;
}

// Check if a ToolResponse should be hidden from user output
bool isHiddenToolResponse(string toolName) {
    return hiddenToolNames.canFind(toolName);
}

private:

/// Replaces invalid UTF-8 sequences in s with U+FFFD (in place).
/// Fast path: if s is already valid UTF-8 it is left untouched (no copy).
/// Returns: true if s was modified.
bool sanitizeField(ref string s) @safe {
    if (s.empty)
        return false;
    try {
        validate(s); // O(n) scan; throws UTFException on the first bad byte
        return false;
    } catch (UTFException) {
        // byUTF!char on char[] is a byte pass-through (no validation);
        // decoding to dchar replaces invalid sequences with U+FFFD.
        s = s.byUTF!dchar.toUTF8;
        return true;
    }
}

// Tools that should not be displayed to the user
enum hiddenToolNames = ["taskDone"];

size_t[Role] RoleLength;

shared static this() {
    import std.traits : EnumMembers;

    {
        size_t[Role] tmp;
        foreach (a; [EnumMembers!Role]) {
            tmp[a] = a.to!string.length;
        }
        RoleLength = tmp;
    }
}

// --- Test: TurnIDs are unique and strictly increasing within one Chat ---
unittest {
    auto chat = Chat();
    assert(chat.currentTurnId() == 0);
    assert(chat.nextTurnId() == 0);

    long prev = chat.allocateTurnId();
    assert(prev == 1);
    foreach (i; 0 .. 10) {
        const id = chat.allocateTurnId();
        assert(id == prev + 1);
        prev = id;
    }
    assert(chat.nextTurnId() == 11);
}

// --- Test: beginNewTurn allocates a fresh ID and opens the turn ---
unittest {
    auto chat = Chat();
    assert(chat.beginNewTurn() == 1);
    assert(chat.currentTurnId() == 1);
    assert(chat.nextTurnId() == 1);
    // Pin I-1 semantics: the counter is prefix-allocated (++nextTurnId_), so
    // nextTurnId() equals currentTurnId() right after an allocation and the
    // next allocation is strictly greater. Task 3 seeds from
    // max(header, max stamp) with no +1.
    assert(chat.nextTurnId() == chat.currentTurnId());
    assert(chat.beginNewTurn() == 2);
    assert(chat.currentTurnId() == 2);
    assert(chat.nextTurnId() == 2);
}

// --- Test: clear() resets currentTurnId_ to 0 but never moves nextTurnId_ ---
unittest {
    auto chat = Chat();
    chat.setSystemPrompt("system");
    chat.beginNewTurn();
    assert(chat.currentTurnId() == 1);

    chat.clear();
    assert(chat.currentTurnId() == 0);
    assert(chat.nextTurnId() == 1); // untouched: the no-reuse guarantee

    // The next turn gets a strictly larger ID than everything ever used.
    assert(chat.beginNewTurn() == 2);
}

// --- Test: two Chat instances run independent counter sequences ---
unittest {
    auto a = Chat();
    auto b = Chat();
    assert(a.allocateTurnId() == 1);
    assert(a.allocateTurnId() == 2);
    assert(b.allocateTurnId() == 1); // overlap across chats is allowed (L1)
    assert(b.allocateTurnId() == 2);
    assert(a.allocateTurnId() == 3); // allocations in b never affect a
    assert(a.nextTurnId() == 3);
    assert(b.nextTurnId() == 2);
}

// --- Test: turnIdOf reads the typed field (0 = unstamped, no JSON involved) ---
unittest {
    auto m = Message(Role.user, userQuery: true, content: "q", thinking: null);
    assert(turnIdOf(Chat.MessageT(m)) == 0); // default-constructed: unstamped
    m.turnId = 7;
    assert(turnIdOf(Chat.MessageT(m)) == 7);

    auto t = ToolMessage(null, JSONValue([JSONValue("call1")]));
    t.turnId = 3;
    assert(turnIdOf(Chat.MessageT(t)) == 3);

    auto r = ToolResponse("out", "call1", "tool", true);
    r.turnId = 4;
    assert(turnIdOf(Chat.MessageT(r)) == 4);

    auto v = VisionMessage("look", "data:image/png;base64,abc");
    v.turnId = 5;
    assert(turnIdOf(Chat.MessageT(v)) == 5);
}

// --- Test: turnRangeOf returns (min, max) over a slice ---
unittest {
    auto m1 = Message(Role.user, userQuery: true, content: "a", thinking: null);
    m1.turnId = 5;
    auto m2 = Message(Role.assistant, userQuery: false, content: "b", thinking: null);
    m2.turnId = 9;
    auto t = ToolMessage(null, JSONValue([JSONValue("call1")]));
    t.turnId = 2;

    Chat.MessageT[] slice = [Chat.MessageT(m1), Chat.MessageT(t), Chat.MessageT(m2)];
    const range = turnRangeOf(slice);
    assert(range.turnStart == 2);
    assert(range.turnEnd == 9);

    Chat.MessageT[] none;
    const emptyRange = turnRangeOf(none);
    assert(emptyRange.turnStart == 0);
    assert(emptyRange.turnEnd == 0);
}

// --- Test: add() stamps turns per the A3 policy ---
unittest {
    auto chat = Chat();
    chat.setSystemPrompt("sys");
    assert(turnIdOf(chat.getMessages[0]) == 0); // system prompt: no turn (A3)

    chat.addUserQuery("question one");
    const q1 = turnIdOf(chat.getMessages[1]);
    assert(q1 == 1);
    assert(chat.currentTurnId() == 1);

    // Everything produced while answering continues the current turn.
    chat.add(ToolMessage("thinking", JSONValue([JSONValue("call")])));
    chat.add(ToolResponse("output", "call-1", "tool", true));
    chat.add(VisionMessage("look", "data:image/png;base64,x"));
    chat.add(Message(Role.user, userQuery: false, content: "nudge", thinking: null));
    chat.add(Message(Role.assistant, userQuery: false, content: "answer", thinking: null));
    foreach (i; 1 .. 7) {
        assert(turnIdOf(chat.getMessages[i]) == q1);
    }

    // The next user query opens a new turn.
    chat.addUserQuery("question two");
    const q2 = turnIdOf(chat.getMessages[7]);
    assert(q2 == q1 + 1);
    assert(chat.currentTurnId() == q2);
    assert(chat.nextTurnId() == q2); // prefix counter: last allocated == current
}

// --- Test: clear() resets the current turn but never the counter (A3) ---
unittest {
    auto chat = Chat();
    chat.addUserQuery("q");
    assert(chat.currentTurnId() == 1);

    chat.clear();
    assert(chat.currentTurnId() == 0);
    assert(chat.nextTurnId() == 1); // counter untouched

    chat.addUserQuery("q2");
    assert(chat.currentTurnId() == 2); // strictly larger: no reuse
}

// --- Test: toSaveJson/fromJson round-trip preserves turn_id in save_data ---
unittest {
    auto m = Message(Role.user, userQuery: true, content: "q", thinking: null);
    m.turnId = 3;
    auto j = m.toSaveJson;
    assert(j["save_data"]["turn_id"].integer == 3);
    assert(("turn_id" in j) is null); // no new top-level keys
    Message m2;
    m2.fromJson(j);
    assert(m2.turnId == 3);

    auto tm = ToolMessage("think", JSONValue([JSONValue("call")]));
    tm.turnId = 5;
    auto tj = tm.toSaveJson;
    assert(tj["save_data"]["turn_id"].integer == 5);
    assert(("turn_id" in tj) is null);
    ToolMessage tm2;
    tm2.fromJson(tj);
    assert(tm2.turnId == 5);

    auto tr = ToolResponse("out", "call-1", "tool", true);
    tr.turnId = 7;
    auto rj = tr.toSaveJson;
    assert(rj["save_data"]["turn_id"].integer == 7);
    assert(("turn_id" in rj) is null);
    ToolResponse tr2;
    tr2.fromJson(rj);
    assert(tr2.turnId == 7);

    auto v = VisionMessage("look", "data:image/png;base64,abc");
    v.turnId = 9;
    auto vj = v.toSaveJson;
    assert(vj["save_data"]["turn_id"].integer == 9);
    assert(("turn_id" in vj) is null);
    VisionMessage v2;
    v2.fromJson(vj);
    assert(v2.turnId == 9);
}

// --- Test: the in-memory saveData AA is never mutated by toSaveJson (A5) ---
unittest {
    auto m = Message(Role.user, userQuery: true, content: "q", thinking: null);
    assert(m.saveData.type == JSONType.object); // ctor stored save_data["user"]
    m.turnId = 4;

    const before = m.saveData.toString;
    auto j = m.toSaveJson;
    assert(j["save_data"]["turn_id"].integer == 4);
    assert(m.saveData.toString == before); // in-memory AA untouched
    assert(("turn_id" in m.saveData) is null);
}

// --- Test: unstamped message gains save_data with turn_id on save ---
unittest {
    auto m = Message(Role.assistant, userQuery: false, content: "ans", thinking: null);
    m.turnId = 2;
    assert(m.saveData == JSONValue.init); // no saveData at all

    auto j = m.toSaveJson;
    assert(j["save_data"]["turn_id"].integer == 2);

    auto m2 = Message(Role.assistant, userQuery: false, content: "ans", thinking: null);
    m2.fromJson(j);
    assert(m2.turnId == 2);
}

// --- Test: pure-legacy file gains boundaries at user-query positions (A5) ---
unittest {
    auto chat = Chat();
    auto doc = parseJSON(`{
        "messages": [
            {"role": "assistant", "content": "hello"},
            {"role": "user", "content": "first", "save_data": {"user": true}},
            {"role": "assistant", "content": "ans1"},
            {"role": "tool", "content": "out", "tool_call_id": "c1", "name": "t"},
            {"role": "user", "content": "second", "save_data": {"user": true}},
            {"role": "assistant", "content": "ans2"}
        ]
    }`);
    chat.load(doc);

    auto msgs = chat.getMessages; // [system, hello, first, ans1, tool, second, ans2]
    assert(msgs.length == 7);
    assert(turnIdOf(msgs[0]) == 0); // system prompt
    assert(turnIdOf(msgs[1]) == 0); // before the first user query
    assert(turnIdOf(msgs[2]) == 1); // first query opens turn 1
    assert(turnIdOf(msgs[3]) == 1);
    assert(turnIdOf(msgs[4]) == 1);
    assert(turnIdOf(msgs[5]) == 2);
    assert(turnIdOf(msgs[6]) == 2);
    assert(chat.currentTurnId() == 2);
    assert(chat.nextTurnId() == 2);

    // Post-load continuation: the next turn gets a strictly greater ID.
    chat.addUserQuery("third");
    assert(turnIdOf(chat.getMessages[7]) == 3);
    assert(chat.currentTurnId() == 3);
}

// --- Test: partially stamped file — gaps inherit the previous turn (H2) ---
unittest {
    auto chat = Chat();
    auto doc = parseJSON(`{
        "messages": [
            {"role": "user", "content": "q1", "save_data": {"user": true, "turn_id": 4}},
            {"role": "assistant", "content": "ans1"},
            {"role": "tool", "content": "out", "tool_call_id": "c1", "name": "t"},
            {"role": "user", "content": "q2", "save_data": {"user": true, "turn_id": 9}},
            {"role": "assistant", "content": "ans2"}
        ]
    }`);
    chat.load(doc);

    auto msgs = chat.getMessages; // [system, q1, ans1, tool, q2, ans2]
    assert(msgs.length == 6);
    assert(turnIdOf(msgs[1]) == 4); // existing stamps kept, never re-stamped
    assert(turnIdOf(msgs[2]) == 4); // gap inherits the previous turn (H2)
    assert(turnIdOf(msgs[3]) == 4);
    assert(turnIdOf(msgs[4]) == 9);
    assert(turnIdOf(msgs[5]) == 9);
    assert(chat.nextTurnId() == 9);
    assert(chat.currentTurnId() == 9);

    // The walk is monotonic (I1): IDs never decrease in file order.
    long prev = 0;
    foreach (m; msgs) {
        const id = turnIdOf(m);
        assert(id >= prev);
        prev = id;
    }

    // Post-load continuation continues above the high-water mark.
    chat.addUserQuery("q3");
    assert(turnIdOf(chat.getMessages[6]) == 10);
}
// --- Test: unstamped prefix in a mixed file takes the first stamped ID (I1) ---
unittest {
    // Live shape: /compact persists an unstamped merged summary before the
    // stamped X/Y tail (summary_agent newHistory) and the next commit writes
    // it back.
    auto chat = Chat();
    auto doc = parseJSON(`{
        "messages": [
            {"role": "assistant", "content": "summary"},
            {"role": "user", "content": "q9", "save_data": {"user": true, "turn_id": 9}},
            {"role": "assistant", "content": "ans9"},
            {"role": "user", "content": "q10", "save_data": {"user": true, "turn_id": 10}}
        ]
    }`);
    chat.load(doc);

    auto msgs = chat.getMessages; // [system, summary, q9, ans9, q10]
    assert(msgs.length == 5);
    assert(turnIdOf(msgs[1]) == 9); // prefix joins the first stamped turn
    assert(turnIdOf(msgs[2]) == 9);
    assert(turnIdOf(msgs[3]) == 9); // mid-file gap inherits
    assert(turnIdOf(msgs[4]) == 10);
    assert(chat.nextTurnId() == 10); // no counter ID burned (H2)
    assert(chat.currentTurnId() == 10);

    // I1: monotonic in file order.
    long prev = 0;
    foreach (m; msgs) {
        const id = turnIdOf(m);
        assert(id >= prev);
        prev = id;
    }

    // Post-load continuation continues above the high-water mark.
    chat.addUserQuery("next");
    assert(chat.currentTurnId() == 11);
}

// --- Test: legacy {"user": false} does not open a turn (A5.5) ---
unittest {
    auto chat = Chat();
    auto doc = parseJSON(`{
        "messages": [
            {"role": "user", "content": "nudge", "save_data": {"user": false}},
            {"role": "assistant", "content": "ans"},
            {"role": "user", "content": "real", "save_data": {"user": true}},
            {"role": "assistant", "content": "ans2"}
        ]
    }`);
    chat.load(doc);

    auto msgs = chat.getMessages; // [system, nudge, ans, real, ans2]
    assert(msgs.length == 5);
    assert(turnIdOf(msgs[1]) == 0); // {"user": false}: no boundary, stays 0
    assert(turnIdOf(msgs[2]) == 0);
    assert(turnIdOf(msgs[3]) == 1); // {"user": true}: opens turn 1
    assert(turnIdOf(msgs[4]) == 1);
    assert(chat.currentTurnId() == 1);
}


// --- Test: vision message round-tripped through fromUser keeps turn_id (M1) ---
unittest {
    auto entry = parseJSON(`{
        "role": "user",
        "content": [
            {"type": "text", "text": "look at this"},
            {"type": "image_url", "image_url": {"url": "data:image/png;base64,abc"}}
        ],
        "save_data": {"turn_id": 12}
    }`);
    auto loaded = fromUser(entry);
    assert(turnIdOf(loaded) == 12);
    auto j = loaded.match!((VisionMessage v) => v.toSaveJson, (_) => JSONValue.init);
    assert(j["save_data"]["turn_id"].integer == 12);
}

// --- Test: corrupt entries are skipped without a crash ---
unittest {
    auto chat = Chat();
    auto doc = parseJSON(`{
        "messages": [
            {"role": "user", "content": "q1", "save_data": {"user": true}},
            {"role": "user", "content": 42},
            {"role": "user", "content": "q2", "save_data": "not-an-object"},
            {"role": "assistant", "content": "ans"}
        ]
    }`);
    chat.load(doc); // must not throw

    auto msgs = chat.getMessages; // [system, q1, corrupt, q2, ans]
    assert(msgs.length == 5);
    assert(turnIdOf(msgs[1]) == 1);
    assert(turnIdOf(msgs[2]) == 1);
    assert(turnIdOf(msgs[3]) == 1); // non-object save_data: no turn_id, no crash
    assert(turnIdOf(msgs[4]) == 1);
}

// --- Test: header next_turn_id exceeds message stamps (evicted-turn continuity) ---
unittest {
    auto chat = Chat();
    auto doc = parseJSON(`{
        "next_turn_id": 42,
        "messages": [
            {"role": "user", "content": "q1", "save_data": {"user": true, "turn_id": 5}},
            {"role": "assistant", "content": "ans"}
        ]
    }`);
    chat.load(doc);

    assert(chat.nextTurnId() == 42); // seeded from the header high-water mark
    assert(turnIdOf(chat.getMessages[1]) == 5);
    assert(turnIdOf(chat.getMessages[2]) == 5);
    chat.addUserQuery("q2");
    assert(chat.currentTurnId() == 43); // continues above the header
}

// --- Test: load() without messages keeps a clean counter ---
unittest {
    auto chat = Chat();
    auto doc = parseJSON(`{"messages": []}`);
    chat.load(doc);
    assert(chat.currentTurnId() == 0);
    assert(chat.nextTurnId() == 0);
    chat.addUserQuery("first");
    assert(chat.currentTurnId() == 1);
}

// --- Test: header seed never moves the counter backwards (I3) ---
unittest {
    auto chat = Chat();
    chat.addUserQuery("warm-up");
    chat.addUserQuery("warm-up 2");
    assert(chat.nextTurnId() == 2);

    // Loading an older/other file must never decrease the counter.
    auto doc = parseJSON(`{
        "next_turn_id": 1,
        "messages": [
            {"role": "user", "content": "q1", "save_data": {"user": true, "turn_id": 1}}
        ]
    }`);
    chat.load(doc);
    assert(chat.nextTurnId() == 2); // unchanged: I3

    chat.addUserQuery("next");
    assert(chat.currentTurnId() == 3);
}

// --- Test: A4 projections split synthetic turns into dialogue and trace ---
unittest {
    auto chat = Chat();
    chat.setSystemPrompt("sys"); // system prompt: neither projection

    // Turn 1: query -> tool call -> tool response -> final answer (taskDone).
    chat.addUserQuery("what is 2+2?");
    chat.add(ToolMessage("computing", JSONValue([JSONValue("call-math")])));
    chat.add(ToolResponse("4", "call-math", "math", true));
    JSONValue sd;
    sd["taskDoneAnswer"] = JSONValue("The answer is 4.");
    chat.add(ToolMessage("final reasoning", JSONValue([JSONValue("call-done")]),
            JSONValue.init, sd));
    chat.add(ToolResponse("done", "call-done", "taskDone", true));

    // Turn 2: query -> direct assistant final with content + thinking (H1).
    chat.addUserQuery("explain it");
    chat.add(Message(Role.assistant, userQuery: false, content: "Because 2 and 2 make 4.",
            thinking: "walk the user through the addition"));

    auto dialogue = chat.getDialogueHistory;
    auto trace = chat.getReasoningTrace;

    // Dialogue: query1, final-answer ToolMessage, query2, assistant final.
    assert(dialogue.length == 4);
    assert(dialogue[0].match!((Message m) => m.isUserQuery && m.content == "what is 2+2?",
            (_) => false));
    assert(dialogue[1].match!((ToolMessage m) => m.isFinalAnswer(), (_) => false));
    assert(dialogue[2].match!((Message m) => m.isUserQuery && m.content == "explain it",
            (_) => false));
    assert(dialogue[3].match!((Message m) => m.content == "Because 2 and 2 make 4.",
            (_) => false));

    // Trace: non-final tool call, both tool responses, and the assistant final
    // that carries thinking (dual classification, H1).
    assert(trace.length == 4);
    assert(trace[0].match!((ToolMessage m) => !m.isFinalAnswer(), (_) => false));
    assert(trace[1].match!((ToolResponse m) => m.toolName == "math", (_) => false));
    assert(trace[2].match!((ToolResponse m) => m.toolName == "taskDone", (_) => false));
    assert(trace[3].match!((Message m) => m.thinking == "walk the user through the addition",
            (_) => false));

    // Canonical order and TurnIDs preserved in both projections.
    assert(turnIdOf(dialogue[0]) == 1);
    assert(turnIdOf(dialogue[1]) == 1);
    assert(turnIdOf(dialogue[2]) == 2);
    assert(turnIdOf(dialogue[3]) == 2);
    assert(turnIdOf(trace[0]) == 1);
    assert(turnIdOf(trace[1]) == 1);
    assert(turnIdOf(trace[2]) == 1);
    assert(turnIdOf(trace[3]) == 2);
    assert(turnRangeOf(dialogue).turnStart == 1 && turnRangeOf(dialogue).turnEnd == 2);
    assert(turnRangeOf(trace).turnStart == 1 && turnRangeOf(trace).turnEnd == 2);
}

// --- Test: content+thinking assistant Message appears in both projections (H1) ---
unittest {
    auto chat = Chat();
    chat.addUserQuery("q");
    chat.add(Message(Role.assistant, userQuery: false, content: "final text",
            thinking: "the reasoning behind it"));

    auto dialogue = chat.getDialogueHistory;
    auto trace = chat.getReasoningTrace;

    // Dual classification: the whole entry appears in both projections —
    // Phase 1 indexes the content (and strips thinking), Phase 2 uses the
    // thinking. One message cannot be split across projections.
    assert(dialogue.length == 2); // query + assistant final
    assert(dialogue[1].match!((Message m) => m.content == "final text"
            && m.thinking == "the reasoning behind it", (_) => false));
    assert(trace.length == 1);
    assert(trace[0].match!((Message m) => m.content == "final text"
            && m.thinking == "the reasoning behind it", (_) => false));
}

// --- Test: harness control traffic appears in neither projection (H1) ---
unittest {
    auto chat = Chat();
    chat.addUserQuery("q");
    // addContinue / addKeepReasoning nudges and feedback warnings
    // (agent:189/220/533) are user-role Messages with userQuery == false.
    chat.add(Message(Role.user, userQuery: false, content: "SYSTEM NUDGE", thinking: null));
    chat.add(Message(Role.user, userQuery: false, content: "feedback warning", thinking: null));
    // H1 is categorical: even a harness nudge that carried thinking stays out.
    chat.add(Message(Role.user, userQuery: false, content: "nudge with thinking",
            thinking: "harness traffic must never leak into the trace"));
    chat.add(Message(Role.assistant, userQuery: false, content: "ans", thinking: null));

    auto dialogue = chat.getDialogueHistory;
    auto trace = chat.getReasoningTrace;

    assert(dialogue.length == 2); // query + assistant final only
    assert(dialogue[0].match!((Message m) => m.isUserQuery, (_) => false));
    assert(dialogue[1].match!((Message m) => m.content == "ans", (_) => false));
    assert(trace.length == 0); // no tools, no thinking strings
}

// --- Test: thinking-only assistant Message lands in the trace only ---
unittest {
    auto chat = Chat();
    chat.addUserQuery("q");
    // A stream can yield reasoning without text: agent:499-501 adds the
    // Message when content or reasoning is non-empty, so this shape is live.
    chat.add(Message(Role.assistant, userQuery: false, content: null,
            thinking: "reasoning without a text response"));

    auto dialogue = chat.getDialogueHistory;
    auto trace = chat.getReasoningTrace;

    assert(dialogue.length == 1); // only the query — no final text response
    assert(dialogue[0].match!((Message m) => m.isUserQuery, (_) => false));
    assert(trace.length == 1); // the thinking string is reasoning material
    assert(trace[0].match!((Message m) => m.thinking == "reasoning without a text response",
            (_) => false));
}

// --- Test: taskDoneAnswer ToolMessage is dialogue; non-final ToolMessage is trace ---
unittest {
    auto chat = Chat();
    chat.addUserQuery("q");
    chat.add(ToolMessage("call reasoning", JSONValue([JSONValue("call-1")]))); // non-final
    JSONValue sd;
    sd["taskDoneAnswer"] = JSONValue("final answer text");
    chat.add(ToolMessage("done reasoning", JSONValue([JSONValue("call-done")]),
            JSONValue.init, sd)); // final

    auto dialogue = chat.getDialogueHistory;
    auto trace = chat.getReasoningTrace;

    assert(dialogue.length == 2); // query + final ToolMessage
    assert(dialogue[1].match!((ToolMessage m) => m.isFinalAnswer(), (_) => false));
    assert(trace.length == 1); // the non-final ToolMessage only
    assert(trace[0].match!((ToolMessage m) => !m.isFinalAnswer(), (_) => false));
}

// --- Test: VisionMessage is not classified by A4 (neither projection) ---
unittest {
    auto chat = Chat();
    chat.beginNewTurn(); // vision turns open explicitly (Task 6)
    chat.add(VisionMessage("what is in this image?", "data:image/png;base64,abc"));

    assert(chat.getDialogueHistory.length == 0);
    assert(chat.getReasoningTrace.length == 0);
}

// --- Test: empty and system-only chats project to nothing ---
unittest {
    auto chat = Chat();
    assert(chat.getDialogueHistory.length == 0);
    assert(chat.getReasoningTrace.length == 0);

    chat.setSystemPrompt("sys");
    assert(chat.getDialogueHistory.length == 0);
    assert(chat.getReasoningTrace.length == 0);
}

// --- Test: header next_turn_id lagging the message stamps self-heals (R6) ---
unittest {
    // Crash between chat mutation and commit: the messages carry stamps up
    // to 9 but the header still holds the previous commit's 3. Seeding from
    // max(header, max stamp) keeps the sequence above everything persisted,
    // so IDs never regress or get re-used.
    auto chat = Chat();
    auto doc = parseJSON(`{
        "next_turn_id": 3,
        "messages": [
            {"role": "user", "content": "q7", "save_data": {"user": true, "turn_id": 7}},
            {"role": "assistant", "content": "a7"},
            {"role": "user", "content": "q9", "save_data": {"user": true, "turn_id": 9}},
            {"role": "assistant", "content": "a9"}
        ]
    }`);
    chat.load(doc);

    assert(chat.nextTurnId() == 9); // max stamp wins over the stale header
    assert(chat.currentTurnId() == 9);
    chat.addUserQuery("q10");
    assert(chat.currentTurnId() == 10); // strictly above the max persisted stamp
}

// --- Test: stamping does not disturb lastResponses/prevIndex tracking ---
unittest {
    auto chat = Chat();
    chat.setSystemPrompt("sys");
    chat.addUserQuery("q1"); // opens turn 1
    chat.resetResponseIndex(); // prevIndex = 2: the response window starts here

    // The response to q1: two assistant messages, both stamped turn 1.
    chat.add(Message(Role.assistant, userQuery: false, content: "a1", thinking: null));
    chat.add(Message(Role.assistant, userQuery: false, content: "a1b", thinking: null));

    auto slice = chat.lastResponses;
    assert(slice.length == 2);
    assert(turnIdOf(slice[0]) == 1);
    assert(turnIdOf(slice[1]) == 1);

    // q2 opens turn 2. The window still starts at prevIndex; the slice keeps
    // each message's own stamp and stays I1-monotonic.
    chat.addUserQuery("q2");
    chat.add(Message(Role.assistant, userQuery: false, content: "a2", thinking: null));
    slice = chat.lastResponses;
    assert(slice.length == 4); // a1, a1b, q2, a2
    long prev = 0;
    foreach (m; slice) {
        const id = turnIdOf(m);
        assert(id >= prev);
        prev = id;
    }
    assert(turnIdOf(slice[2]) == 2);
    assert(turnIdOf(slice[3]) == 2);

    // resetResponseIndex empties the window: stamping never moved prevIndex.
    chat.resetResponseIndex();
    assert(chat.lastResponses is null);
}

// --- Test: setHistory preserves an I1-sorted replacement (compression shape) ---
unittest {
    // The shape compress() hands to setHistory: the system prompt (0), a
    // merged summary stamped with the summarized slice's turnEnd, then the
    // kept X/Y tail whose ids are >= turnEnd. setHistory is a raw
    // replacement - it neither sorts nor stamps, so I1 ordering is the
    // caller's contract.
    auto chat = Chat();
    chat.setSystemPrompt("sys");
    chat.addUserQuery("q1"); // turn 1
    chat.add(Message(Role.assistant, userQuery: false, content: "a1", thinking: null));
    chat.addUserQuery("q2"); // turn 2
    chat.add(Message(Role.assistant, userQuery: false, content: "a2", thinking: null));

    auto summary = Message(Role.assistant, userQuery: false,
            content: "merged summary of turn 1", thinking: null);
    summary.turnId = 1; // turnEnd of the summarized slice (C2)
    Chat.MessageT[] replacement = [
        chat.getMessages[0], // system prompt, turn 0
        Chat.MessageT(summary),
        chat.getMessages[3], // q2, turn 2
        chat.getMessages[4], // a2, turn 2
    ];
    chat.setHistory(replacement);

    assert(chat.length == 4);
    long prev = 0;
    foreach (m; chat.getMessages) {
        const id = turnIdOf(m);
        assert(id >= prev, "history must stay (turn_id, position)-sorted (I1)");
        prev = id;
    }
    assert(turnIdOf(chat.getMessages[1]) == 1);
    assert(turnIdOf(chat.getMessages[2]) == 2);
    assert(turnIdOf(chat.getMessages[3]) == 2);
}

// ===================== Tests for Chat.sanitizeHistory =====================

/// Test: sanitizeHistory on an empty chat returns 0.
unittest {
    Chat chat;
    assert(chat.sanitizeHistory() == 0);
}

/// Test: sanitizeHistory returns 0 for a clean chat and leaves valid
/// multibyte UTF-8 untouched (no copy).
unittest {
    Chat chat;
    chat.add(Message(Role.system, userQuery: false, content: "sys", thinking: null));
    chat.add(Message(Role.user, userQuery: true, content: "héllo 🚀", thinking: "über dacht"));
    chat.add(Message(Role.assistant, userQuery: false, content: "ok", thinking: "nope"));
    chat.add(ToolMessage("dacht", parseJSON("[{\"id\": \"c1\", \"type\": \"function\", "
            ~ "\"function\": {\"name\": \"t\", \"arguments\": \"{}\"}}]")));
    chat.add(ToolResponse("result", "c1", "t", true));
    chat.add(VisionMessage("caption", "data:image/png;base64,AAAA"));

    assert(chat.sanitizeHistory() == 0);
    assert(chat.length == 6);
    chat.getMessages[1].match!((Message a) {
        assert(a.content == "héllo 🚀");
        assert(a.thinking == "über dacht");
    }, (ToolMessage _) {}, (ToolResponse _) {}, (VisionMessage _) {});
}

/// Test: the original crash scenario — a poisoned ToolResponse (raw bytes from
/// command output) is healed in place; no message is discarded.
unittest {
    Chat chat;
    chat.add(Message(Role.system, userQuery: false, content: "sys", thinking: null));
    chat.add(Message(Role.user, userQuery: true, content: "run printf", thinking: null));
    chat.add(ToolResponse("\x80\x81", "call_42", "executeCommand", true));
    auto len = chat.length;

    auto n = chat.sanitizeHistory();

    assert(n == 1);
    assert(chat.length == len);
    chat.getMessages[2].match!((Message _) {}, (ToolMessage _) {}, (ToolResponse a) {
        assert(a.content == "\uFFFD\uFFFD");
        assert(a.toolCallId == "call_42"); // valid fields untouched
        assert(a.toolName == "executeCommand");
    }, (VisionMessage _) {});
}

/// Test: sanitizeHistory heals every message type and counts each modified
/// message; result is idempotent.
unittest {
    Chat chat;
    chat.add(Message(Role.system, userQuery: false, content: "sys", thinking: null));
    chat.add(Message(Role.user, userQuery: true, content: "a\x80b", thinking: "t\xFFx"));
    chat.add(ToolMessage("think\x80", parseJSON("[{\"id\": \"c1\", \"type\": \"function\", "
            ~ "\"function\": {\"name\": \"t\", \"arguments\": \"{}\"}}]")));
    chat.add(ToolResponse("out\x80\x81", "id\xFF", "name\x80", true));
    chat.add(VisionMessage("cap\x80", "data:img\xFF"));

    auto n = chat.sanitizeHistory();

    assert(n == 4); // system message clean; the other four modified
    assert(chat.length == 5); // no messages discarded

    auto msgs = chat.getMessages;
    msgs[1].match!((Message a) {
        assert(a.content == "a\uFFFDb");
        // 0xFF is an invalid lead byte; the decoder consumes the following
        // byte ('x') as part of the malformed sequence (see utility.d tests).
        assert(a.thinking == "t\uFFFD");
    }, (ToolMessage _) {}, (ToolResponse _) {}, (VisionMessage _) {});
    msgs[2].match!((Message _) {}, (ToolMessage a) {
        assert(a.thinking == "think\uFFFD");
    }, (ToolResponse _) {}, (VisionMessage _) {});
    msgs[3].match!((Message _) {}, (ToolMessage _) {}, (ToolResponse a) {
        assert(a.content == "out\uFFFD\uFFFD");
        assert(a.toolCallId == "id\uFFFD");
        assert(a.toolName == "name\uFFFD");
    }, (VisionMessage _) {});
    msgs[4].match!((Message _) {}, (ToolMessage _) {}, (ToolResponse _) {}, (VisionMessage a) {
        assert(a.content == "cap\uFFFD");
        assert(a.imageDataUrl == "data:img\uFFFD");
    });

    assert(chat.sanitizeHistory() == 0); // idempotent: healed history is valid
}
