/// Known tag vocabulary for the @Function UDA.
module llm.tool_call.tags;

import std.algorithm : map, filter;
import std.array : join, array;
import std.conv : to;
import std.traits : EnumMembers;

/// Tags recognized at registration time. Adding a tag = add an enum member.
/// Runtime tags (external/MCP) are free-form strings that skip
/// this enum: the enum check is typo protection for tags written in source.
enum KnownToolTag {
    workarea,
    rag,
    memory,
    dialogue,
    reasoning,
    metrics,
    mcp,
    encoding,
    vision,
    env,
    pipeline
}

/// Tags in `tags` that are not members of KnownToolTag (registration-time
/// typo check: WARN not error, built-in path only). Pure; the
/// RegisterLlmFunctions mixin logs whatever this returns.
string[] unknownToolTags(string[] tags) @safe pure {
    import my.set;

    auto known = toSet([EnumMembers!KnownToolTag].map!(a => a.to!string));
    return tags.filter!(a => a !in known).array;
}

/// Comma-joined KnownToolTag member names, for warning text.
string knownToolTagNames() @safe pure {
    return [EnumMembers!KnownToolTag].map!(a => a.to!string).join(", ");
}
