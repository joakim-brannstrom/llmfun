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

import llm.common.config : ApproxTokenSize;
import llm.utility : getValue;

struct Chat {
    alias MessageT = SumType!(Message, ToolMessage, ToolResponse, VisionMessage);

    private {
        MessageT[] history;
        size_t prevIndex;
    }

    void setSystemPrompt(string x) {
        auto m = Message(Role.system, userQuery: false, content: x, thinking: null);
        if (history.empty)
            history ~= MessageT(m);
        else
            history[0] = m;
    }

    void clear() @safe pure nothrow @nogc {
        if (history.empty)
            return;
        history = history[0 .. 1];
        prevIndex = 1;
    }

    void add(Message m) @safe pure nothrow {
        history ~= MessageT(m);
    }

    void add(ToolMessage m) @safe pure nothrow {
        history ~= MessageT(m);
    }

    void add(ToolResponse m) @safe pure nothrow {
        history ~= MessageT(m);
    }

    void add(VisionMessage m) @safe pure nothrow {
        history ~= MessageT(m);
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

    long approxContextSize() @safe nothrow {
        long ctx;
        try {
            foreach (msg; history) {
                // dfmt off
                ctx += msg.match!(
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
            }
        } catch (Exception e) {
        }
        return ctx;
    }

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
            logger.tracef("Loaded previous chat history. Size %s->%s", startLen, history.length);
        } catch (Exception e) {
            logger.trace(e).collectException;
            logger.trace(e.msg).collectException;
        }
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

struct VisionMessage {
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

    bool isUserQuery() @safe const nothrow {
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
        if (saveData != JSONValue.init) {
            j["save_data"] = saveData;
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
            rval = VisionMessage(text, imageDataUrl, metadata);
        } else {
            string thinking = getValue(entry, (v) => v["reasoning_content"].str, null);
            auto metadata = getValue(entry, (v) => v["metadata"], JSONValue.init);
            auto saveData = getValue(entry, (v) => v["save_data"], JSONValue.init);
            rval = Message(Role.user, userQuery: false, content: text,
                    thinking: thinking, metaData: metadata, saveData: saveData);
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
        if (saveData != JSONValue.init) {
            j["save_data"] = saveData;
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
        if (saveData != JSONValue.init) {
            j["save_data"] = saveData;
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
