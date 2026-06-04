module llm.chat;

import logger = std.logger;
import std.algorithm : filter, map, sum, min, canFind;
import std.array : array, replace, appender;
import std.conv : to;
import std.exception : collectException;
import std.format : format;
import std.json : JSONValue, JSONOptions, parseJSON, JSONType;
import std.range : enumerate, isOutputRange, empty;
import std.sumtype : SumType, match;
import llm.utility : ApproxTokenSize;

struct Chat {
    alias MessageT = SumType!(Message, ToolMessage, ToolResponse, VisionMessage);

    private {
        MessageT[] history;
        size_t prevIndex;
    }

    void setSystemPrompt(string x) {
        auto m = Message(Role.system, x);
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
        if (history.empty || prevIndex == history.length)
            return null;
        if (prevIndex >= history.length)
            prevIndex = min(history.length, 1);
        return history[prevIndex .. $];
    }

    MessageT[] getMessages() @safe nothrow {
        return history;
    }

    long approxContextSize() @safe nothrow {

        long ctx;
        try {
            foreach (msg; history) {
                ctx += msg.match!((Message a) {
                    return a.content.length / ApproxTokenSize;
                }, (ToolMessage a) {
                    return a.toolCalls.toString.length / ApproxTokenSize;
                }, (ToolResponse a) { return a.content.length / ApproxTokenSize; },
                        (VisionMessage a) {
                    return a.content.length / ApproxTokenSize;
                });
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
            const startLen = history.length;
            foreach (entry; json["messages"].array) {
                const role = entry["role"].str.to!Role;
                JSONValue metadata;
                if (auto m = "metadata" in entry) {
                    metadata = *m;
                }

                final switch (role) {
                case Role.system:
                    break;
                case Role.assistant:
                    if ("tool_calls" in entry) {
                        history ~= MessageT(ToolMessage(entry["tool_calls"], metadata));
                    } else {
                        history ~= MessageT(Message(role, entry["content"].str, metadata));
                    }
                    break;
                case Role.tool:
                    history ~= MessageT(ToolResponse(content: entry["content"].str,
                            toolCallId: entry["tool_call_id"].str, toolName: entry["name"].str,
                            metadata));
                    break;
                case Role.user:
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
                            history ~= MessageT(VisionMessage(text, imageDataUrl, metadata));
                        } else {
                            history ~= MessageT(Message(role, text, metadata));
                        }
                    } else {
                        history ~= MessageT(Message(role, entry["content"].str, metadata));
                    }
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
                    a.toolCalls.toString), (ToolResponse a) => formattedWrite(w, `[%s, "%s", id:%s, name:%s]`,
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

    string toString() @safe const {
        string imgPreview;
        if (imageDataUrl.length > 60) {
            imgPreview = "[" ~ imageDataUrl[0 .. 57] ~ "...]";
        } else {
            imgPreview = "[" ~ imageDataUrl ~ "]";
        }
        return format!"VisionMessage(content:%s image:%s)"(content, imgPreview);
    }
}

struct Message {
@safe:
    Role role;
    string content;
    JSONValue metadata;

    this(Role role, string content, JSONValue metadata = JSONValue.init) @safe nothrow {
        this.role = role;
        this.content = content;
        this.metadata = metadata;
    }

    size_t length() @safe const nothrow {
        return content.length;
    }

    string toString() @safe const {
        return format!"Message(role:%s content:%s)"(role, content);
    }

    JSONValue toJson() @safe {
        return JSONValue(["role": role.to!string, "content": content]);
    }

    JSONValue toSaveJson() @safe {
        auto j = toJson();
        if (metadata != JSONValue.init) {
            j["metadata"] = metadata;
        }
        return j;
    }

    void fromJson(JSONValue j) @trusted nothrow {
        try {
            this.role = j["role"].str.to!Role;
            this.content = j["content"].str;
        } catch (Exception e) {
            logger.trace(e.msg).collectException;
        }
    }
}

struct ToolMessage {
@safe:
    Role role;
    JSONValue toolCalls;
    JSONValue metadata;

    this(JSONValue toolCalls, JSONValue metadata = JSONValue.init) @safe nothrow {
        this.role = Role.assistant;
        this.toolCalls = toolCalls;
        this.metadata = metadata;
    }

    size_t length() @safe const {
        return toolCalls.toString.length;
    }

    string toString() @safe const {
        return format!"Message(role:%s content:%s)"(role, toolCalls.toString);
    }

    JSONValue toJson() @safe {
        return JSONValue([
            "role": JSONValue(role.to!string),
            "content": JSONValue(null),
            "tool_calls": toolCalls
        ]);
    }

    JSONValue toSaveJson() @safe {
        auto j = toJson();
        if (metadata != JSONValue.init) {
            j["metadata"] = metadata;
        }
        return j;
    }

    void fromJson(JSONValue j) @trusted nothrow {
        try {
            this.role = j["role"].str.to!Role;
            this.toolCalls = j["tool_calls"];
        } catch (Exception e) {
            logger.trace(e.msg).collectException;
        }
    }
}

struct ToolResponse {
@safe:
    Role role;
    string content;
    string toolCallId;
    string toolName;
    JSONValue metadata;

    this(string content, string toolCallId, string toolName, JSONValue metadata = JSONValue.init) @safe nothrow {
        this.role = Role.tool;
        this.content = content;
        this.toolCallId = toolCallId;
        this.toolName = toolName;
        this.metadata = metadata;
    }

    size_t length() @safe const nothrow {
        return content.length;
    }

    string toString() @safe const {
        return format!"Message(role:%s toolName:%s content:%s)"(role, toolName, content);
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
        return j;
    }

    void fromJson(JSONValue j) @trusted nothrow {
        try {
            this.role = j["role"].str.to!Role;
            this.content = j["content"].str;
        } catch (Exception e) {
            logger.trace(e.msg).collectException;
        }
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
