module llm.tool_call;

import logger = std.logger;
import std.algorithm : canFind, filter, map;
import std.conv : text;
import std.json : JSONValue, JSONType, parseJSON, JSONOptions;
import std.range : array;
import std.traits : isIntegral, isFloatingPoint;
import std.typecons : Nullable;

import my.filter : ReFilter;

import llm.config : VisionModelConfig;

interface Context {
}

// UDA use to mark a function as a tool to be used by llm.
struct Function {
    string desc;
}

// UDA to mark a parameter as optional
struct ParamOptional {
}

// UDA to describe a parameter (in the parameter struct)
struct ParamDescription {
    string value;
}

struct ExecuteFuncResult {
    string msg;
    bool success;
}

struct RegParam {
    string name;
    string type;
    string desc;
    bool required;
    /// For array types, specifies the item type (e.g., "string" for string[])
    string itemsType;
}

struct RegFunction {
    string name;
    string desc;
    RegParam[] params;
    ExecuteFuncResult function(Context, JSONValue) callback;
}

struct FunctionCall {
    string name;
    JSONValue args;
}

RegFunction[] getFunctions() @trusted nothrow @nogc {
    return cast(RegFunction[]) registeredFunc;
}

// should only be called called at program start single threaded.
void addFunction(RegFunction f) {
    if (registeredFunc.canFind!(fd => fd.name == f.name)) {
        logger.warningf("Duplicate tool function '%s' registered, ignoring", f.name);
        return;
    }
    registeredFunc ~= cast(shared) f;
}

// Filter the JSON tool descriptions array using ReFilter.
// Only tools whose name matches the filter are returned.
JSONValue filterToolDescriptions(JSONValue allTools, ReFilter filter_) {
    import std.algorithm : filter;

    return JSONValue(allTools.array.filter!(a => filter_.match(a["function"]["name"].str)).array);
}

private ExecuteFuncResult executeFunc(Context ctx, string name, JSONValue args) nothrow {
    try {
        foreach (func; getFunctions.filter!(a => a.name == name)) {
            auto rval = func.callback(ctx, args);
            logger.tracef("call %s with args %s -> %s", name, args, rval.msg);
            return rval;
        }
        return ExecuteFuncResult(i"error: unknown tool $(name)".text, false);
    } catch (Exception e) {
        try {
            return ExecuteFuncResult(i"error: executing tool '$(name)': $(e.msg)".text, false);
        } catch (Exception e) {
        }
    }
    return ExecuteFuncResult("error: should not happen", false);
}

// If the tool name does not match the filter, returns an error.
ExecuteFuncResult executeFunc(Context ctx, string name, JSONValue args, ReFilter filter_) nothrow {
    try {
        if (!filter_.match(name))
            return ExecuteFuncResult(
                    "error: tool '" ~ name ~ "' is not available to this agent", false);
    } catch (Exception e) {
        return ExecuteFuncResult("error: tool '" ~ name ~ "' is not available to this agent", false);
    }
    return executeFunc(ctx, name, args);
}

// Returns: JSON following the OpenAI format
JSONValue descAllFunctions() @safe {
    import std.algorithm : map, filter;
    import std.array : array, empty;

    JSONValue[] rval;
    foreach (func; getFunctions) {
        JSONValue jfunc;
        jfunc["name"] = func.name;
        jfunc["description"] = func.desc;

        auto jparams = JSONValue.emptyObject;
        foreach (p; func.params) {
            auto j = JSONValue.emptyObject;
            j["type"] = p.type;
            if (!p.itemsType.empty) {
                auto items = JSONValue.emptyObject;
                items["type"] = p.itemsType;
                j["items"] = items;
            }
            if (!p.desc.empty) {
                j["description"] = p.desc;
            }
            jparams[p.name] = j;
        }
        jfunc["parameters"] = JSONValue.emptyObject;
        jfunc["parameters"]["type"] = "object";
        jfunc["parameters"]["properties"] = jparams;
        jfunc["parameters"]["required"] = JSONValue(func.params
                .filter!(a => a.required)
                .map!(a => a.name)
                .array);

        JSONValue jwrap;
        jwrap["type"] = "function";
        jwrap["function"] = jfunc;
        rval ~= jwrap;
    }
    return JSONValue(rval);
}

// Register all functions marked by @Function in the module.
mixin template RegisterLlmFunctions() {
    shared static this() {
        import llm.tool_call : addFunction, RegFunction, Function, toParams, initParams;
        import std.array : empty;
        import std.json : JSONValue;
        import std.traits : hasUDA, getUDAs, Parameters, isAggregateType;

        mixin("alias TheModule = " ~ __MODULE__ ~ ";");

        static foreach (moduleMemberName; __traits(allMembers, TheModule)) {
            {
                static if (moduleMemberName != "object" && moduleMemberName != "TheModule") {
                    mixin("alias moduleMember = " ~ moduleMemberName ~ ";");
                    static if (is(typeof(moduleMember) == function)
                            && hasUDA!(moduleMember, Function)) {
                        enum funcDesc = getUDAs!(moduleMember, Function)[0].desc;
                        alias FuncParamTypes = Parameters!moduleMember;
                        static assert(FuncParamTypes.length == 2,
                                "Function " ~ __MODULE__ ~ "." ~ moduleMemberName
                                ~ " must take only two arguments where the second is a struct");
                        alias ParamsT = FuncParamTypes[1];
                        static assert(isAggregateType!ParamsT,
                                "Function " ~ __MODULE__ ~ "." ~ moduleMemberName
                                ~ " second parameter must be an aggregate type but is a "
                                ~ ParamsT.stringof);

                        static ExecuteFuncResult funcCallback(Context ctx, JSONValue args) {
                            auto params = initParams!ParamsT(args, toParams!ParamsT);
                            if (params.errorMsg.empty) {
                                return moduleMember(ctx, params.value);
                            }
                            return ExecuteFuncResult(msg: params.errorMsg, success: false);
                        }

                        addFunction(RegFunction(name: moduleMemberName, desc: funcDesc,
                                params: toParams!ParamsT, callback: &funcCallback));
                    }
                }
            }
        }
    }
}

string baseContextToSpecific(TargetT, string func = __PRETTY_FUNCTION__)() {
    return `auto ctx = cast(` ~ TargetT.stringof ~ `) baseCtx;
    if (ctx is null)
        return ExecuteFuncResult("error: context do not support ` ~ func ~ `", false);
`;
}

RegParam toParam(T)(string name, string desc, bool required) {
    RegParam result;
    result.name = name;
    result.desc = desc;
    result.required = required;

    static if (is(T == string))
        result.type = "string";
    else static if (isIntegral!T || isFloatingPoint!T)
        result.type = "number";
    else static if (is(T == bool))
        result.type = "boolean";
    else static if (is(T == string[])) {
        result.type = "array";
        result.itemsType = "string";
    } else
        static assert(0, "unsupported type " ~ T.stringof);

    return result;
}

auto getJsonValue(T)(JSONValue json) {
    static if (is(T == string))
        return json.str;
    else static if (isIntegral!T)
        return cast(T) json.integer;
    else static if (isFloatingPoint!T)
        return json.floating;
    else static if (is(T == bool))
        return json.boolean;
    else static if (is(T == string[])) {
        string[] result;
        foreach (item; json.array) {
            result ~= item.str;
        }
        return result;
    } else
        static assert(0, "unsupported type " ~ T.stringof);
}

struct InitParams(T) {
    string errorMsg;
    T value;
}

InitParams!ParamsT initParams(ParamsT)(JSONValue json, RegParam[] regParams) {
    import std.algorithm : filter, map;
    import std.conv : text;

    InitParams!ParamsT rval;

    foreach (a; regParams.filter!(a => a.required)) {
        if (a.name !in json) {
            rval.errorMsg = i"error: missing required parameter '$(a.name)'".text;
            return rval;
        }
    }

    bool[string] allFields;
    static foreach (field; __traits(allMembers, ParamsT)) {
        {
            alias FT = typeof(__traits(getMember, rval.value, field));
            allFields[field] = true;
            if (auto v = field in json) {
                try {
                    __traits(getMember, rval.value, field) = getJsonValue!FT(*v);
                } catch (Exception e) {
                    bool success;

                    // The JSON value has the wrong kind for this parameter.
                    // Detect the common double-encoding mistake: a string
                    // whose content is a JSON-encoded array. Name the fix
                    // instead of leaving the caller with a bare type error.
                    static if (is(FT == string[])) {
                        if (v.type == JSONType.string) {
                            // the LLM is retarded and sometimes go into a loop
                            // where it keep on passing a JSON array as a string.
                            // Try to decode the string. If it succeeds use that.
                            try {
                                __traits(getMember, rval.value, field) = getJsonValue!FT(parseJSON(v.str));
                                success = true;
                            } catch(Exception e) {
                            }

                            if (!success) {
                                try {
                                    auto inner = parseJSON(v.str);
                                    if (inner.type == JSONType.array) {
                                        rval.errorMsg = i"error: parameter '$(field)' must be an array of strings, but a string was passed that contains JSON-encoded array text ($(
                                                v.str)). Pass a real JSON array instead, e.g. \"$(field)\": [\"a\", \"b\"]"
                                            .text;
                                        return rval;
                                    }
                                } catch (Exception) {
                                    // inner string is not JSON; report the
                                    // generic mismatch below
                                }
                            }
                        }
                    }
                    if (!success) {
                        rval.errorMsg = i"error: wrong parameter type for '$(field)': received $(v.type), expected $(
                                FT.stringof). $(e.msg)".text;
                        return rval;
                    }
                }
            }
        }
    }

    foreach (key; json.object.byKey) {
        if (key !in allFields) {
            rval.errorMsg = i"error: no such parameter '$(key)'".text;
        }
    }

    return rval;
}

/// Sample params struct used by the initParams unit tests.
private struct TestCmdParams {
    string[] command;
    @ParamOptional string cwd;
}

unittest {
    // Double-encoded array: a string containing JSON array text must produce
    // a hint that names the fix instead of a bare type-mismatch error.
    auto json = parseJSON(`{"command": "[\"cd\", \"llmfun\"]"}`);
    auto params = initParams!TestCmdParams(json, toParams!TestCmdParams);
    assert(params.errorMsg.length > 0);
    assert(params.errorMsg.canFind("array of strings"), params.errorMsg);
    assert(params.errorMsg.canFind("command"), params.errorMsg);

    // A real array converts without error.
    auto okJson = parseJSON(`{"command": ["cd", "llmfun"]}`);
    auto okParams = initParams!TestCmdParams(okJson, toParams!TestCmdParams);
    assert(okParams.errorMsg.length == 0, okParams.errorMsg);
    assert(okParams.value.command == ["cd", "llmfun"]);

    // A plain string (not JSON) for an array parameter gets the generic
    // received/expected message.
    auto strJson = parseJSON(`{"command": "cd llmfun"}`);
    auto strParams = initParams!TestCmdParams(strJson, toParams!TestCmdParams);
    assert(strParams.errorMsg.canFind("received string"), strParams.errorMsg);
    assert(strParams.errorMsg.canFind("string[]"), strParams.errorMsg);
}

RegParam[] toParams(FunctionParamT)() {
    import std.traits : hasUDA, getUDAs;

    RegParam[] rval;
    static foreach (field; FunctionParamT.tupleof) {
        {
            enum name = field.stringof;
            static if (hasUDA!(field, ParamDescription)) {
                enum desc = getUDAs!(field, ParamDescription)[0].value;
            } else {
                enum desc = "";
            }
            enum isOptional = hasUDA!(field, ParamOptional);
            rval ~= toParam!(typeof(field))(name: name, desc: desc, required: !isOptional);
        }
    }

    return rval;
}

private:

shared(RegFunction[]) registeredFunc;
