module llm.tool_call;

import logger = std.logger;
import std.algorithm : canFind, filter, map;
import std.format : format;
import std.json : JSONValue, JSONType, parseJSON, JSONOptions;
import std.range : array;
import std.traits : isIntegral, isFloatingPoint;

import my.filter : ReFilter;

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
        return ExecuteFuncResult(format!"error: unknown tool %s"(name), false);
    } catch (Exception e) {
        try {
            return ExecuteFuncResult(format!"error: executing tool '%s': %s"(name, e.msg), false);
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

package:
string baseContextToSpecific(TargetT, string func = __PRETTY_FUNCTION__)() {
    return `auto ctx = cast(` ~ TargetT.stringof ~ `) baseCtx;
    if (ctx is null)
        return ExecuteFuncResult("error: context do not support ` ~ func ~ `", false);
`;
}

RegParam toParam(T)(string name, string desc, bool required) {
    static if (is(T == string))
        enum type = "string";
    else static if (isIntegral!T || isFloatingPoint!T)
        enum type = "number";
    else static if (is(T == bool))
        enum type = "boolean";
    else
        static assert(0, "unsupported type " ~ T.stringof);

    return RegParam(name: name, type: type, desc: desc, required: required);
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
    else
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
                    rval.errorMsg = i"error: wrong parameter type '$(v.type)': Expected parameter '$(
                            field)' of type $(FT.stringof): $(e.msg)".text;
                    return rval;
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
