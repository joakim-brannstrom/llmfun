module llm.tool_call;

import logger = std.logger;
import std.algorithm : canFind, filter, map;
import std.format : format;
import std.json : JSONValue, JSONType, parseJSON, JSONOptions;
import std.range : array;

import my.filter : ReFilter;

interface Context {
}

// UDA use to mark a function as a tool to be used by llm.
struct Function {
    string desc;
}

struct Param {
    string type;
    string name;
}

// Extracted from functions marked by UDA's and the parameters
struct FunctionDesc {
    string name;
    string desc;
    Param[] params;
    ExecuteFuncResult function(Context, JSONValue) callback;
}

// A function call
struct FunctionCall {
    string name;
    JSONValue args;
}

FunctionDesc[] getFunctions() @trusted nothrow @nogc {
    return cast(FunctionDesc[]) registeredFunc;
}

// only called at program start single threaded.
void addFunction(FunctionDesc f) {
    registeredFunc ~= cast(shared) f;
}

struct ExecuteFuncResult {
    string msg;
    bool success;
}

ExecuteFuncResult executeFunc(Context ctx, string name, JSONValue args) nothrow {
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

/// Filter the JSON tool descriptions array using ReFilter.
/// Only tools whose name matches the filter are returned.
JSONValue filterToolDescriptions(JSONValue allTools, ReFilter filter_) {
    import std.algorithm : filter;

    return JSONValue(allTools.array.filter!(a => filter_.match(a["function"]["name"].str)).array);
}

/// Overload of executeFunc that filters tool execution via ReFilter.
/// If the tool name does not match the filter, returns an error.
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

// JSON following the OpenAI format
JSONValue descAllFunctions() @safe {
    JSONValue[] rval;
    foreach (func; getFunctions) {
        JSONValue jf;
        jf["name"] = func.name;
        jf["description"] = func.desc;

        auto jparams = JSONValue.emptyObject;
        foreach (param; func.params) {
            jparams[param.name] = JSONValue(["type": param.type]);
        }
        jf["parameters"] = [
            "type": JSONValue("object"),
            "properties": jparams,
            "required": JSONValue(func.params.map!(a => a.name).array)
        ];

        JSONValue wrap;
        wrap["type"] = "function";
        wrap["function"] = jf;
        rval ~= wrap;
    }
    return JSONValue(rval);
}

struct ParseFuncCallResult {
    FunctionCall[] calls;
    bool failed;
}

ParseFuncCallResult parseFuncCall(string text) {
    import std.regex : regex, matchAll;

    FunctionCall[] calls;
    bool parseFail;
    static auto pattern = regex(r"<tool_call>\s*(\{.*?\})\s*</tool_call>", "gs");
    foreach (match; matchAll(text, pattern)) {
        try {
            auto j = parseJSON(match[1].map!(c => c < 0x20 ? ' ' : c).array);
            if ("name" in j && "arguments" in j) {
                calls ~= FunctionCall(j["name"].str, j["arguments"]);
            }
        } catch (Exception e) {
            logger.infof("Failed to parse function call: %s", e.msg);
            parseFail = true;
        }
    }
    return ParseFuncCallResult(calls, parseFail);
}

string getJsonGrammar() {
    import std.array : join;

    const functions = getFunctions.map!(a => `"\"` ~ a.name ~ `\""`).join(" | ");

    // json.gbnf
    return `
root ::= "{" ws "\"name\"" ws ":" ws toolname ws "," ws "\"arguments\"" ws ":" ws object ws "}</tool_call>"

toolname ::= ` ~ functions ~ `

value  ::= object | array | string | number | ("true" | "false" | "null") ws

object ::=
  "{" ws (
            string ":" ws value
    ("," ws string ":" ws value)*
  )? "}" ws

array  ::=
  "[" ws (
            value
    ("," ws value)*
  )? "]" ws

string ::=
  "\"" (
    [^"\\] |
    "\\" (["\\/bfnrt] | "u" [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F]) # escapes
  )* "\"" ws

number ::= ("-"? ([0-9] | [1-9] [0-9]*)) ("." [0-9]+)? ([eE] [-+]? [0-9]+)? ws

# Optional space: by convention, applied in this grammar after literal chars when allowed
ws ::= ([ \t\n] ws)?
        `;
}

// Register all functions marked by @Function in the module.
mixin template RegisterLlmFunctions() {
    shared static this() {
        import llm.tool_call : addFunction, FunctionDesc, Function;
        import std.algorithm : among;
        import std.array : join;
        import std.conv : to;
        import std.format : format;
        import std.json : JSONValue, JSONType, JSONOptions;
        import std.traits : hasUDA, getUDAs, ParameterIdentifierTuple,
            Parameters, isIntegral, isFloatingPoint;

        mixin("alias TheModule = " ~ __MODULE__ ~ ";");

        static foreach (memberName; __traits(allMembers, TheModule)) {
            {
                static if (memberName != "object" && memberName != "TheModule") {
                    mixin("alias member = " ~ memberName ~ ";");
                    static if (is(typeof(member) == function) && hasUDA!(member, Function)) {
                        enum funcDesc = getUDAs!(member, Function)[0].desc;
                        alias FuncParamNames = ParameterIdentifierTuple!member[1 .. $];
                        alias FuncParamTypes = Parameters!member[1 .. $];

                        Param[] params;
                        static foreach (i; 0 .. FuncParamNames.length) {
                            {
                                static if (is(FuncParamTypes[i] : string))
                                    enum type = "string";
                                else static if (isIntegral!(FuncParamTypes[i])
                                        || isFloatingPoint!(FuncParamTypes[i]))
                                    enum type = "number";
                                else
                                    enum type = "null";
                                params ~= Param(type: type, name: FuncParamNames[i]);
                            }
                        }

                        static ExecuteFuncResult funcCallback(Context ctx, JSONValue args) {
                            string[] strValues;
                            long[] intValues;
                            double[] floatValues;
                            bool[] boolValues;

                            ExecuteFuncResult makeWarning(size_t i)() {
                                return ExecuteFuncResult(format!"error using tool '%s': wrong parameter type '%s': Expected parameter '%s' of type '%s'"(
                                        memberName, args[FuncParamNames[i]].type,
                                        FuncParamNames[i], FuncParamTypes[i].stringof), false);
                            }

                            static foreach (i; 0 .. FuncParamNames.length) {
                                if (FuncParamNames[i]!in args) {
                                    return ExecuteFuncResult(
                                            format!"error using tool '%s': missing parameter: '%s' of type '%s'"(
                                            memberName, FuncParamNames[i],
                                            FuncParamTypes[i].stringof), false);
                                }

                                strValues ~= "";
                                intValues ~= 0;
                                floatValues ~= 0.0;
                                boolValues ~= false;

                                static if (is(FuncParamTypes[i] : string)) {
                                    if (args[FuncParamNames[i]].type == JSONType.string) {
                                        strValues[$ - 1] = args[FuncParamNames[i]].str;
                                    } else {
                                        return makeWarning!(i)();
                                    }
                                } else static if (isIntegral!(FuncParamTypes[i])) {
                                    if (args[FuncParamNames[i]].type == JSONType.integer) {
                                        intValues[$ - 1] = args[FuncParamNames[i]].integer;
                                    } else {
                                        return makeWarning!(i)();
                                    }
                                } else static if (isFloatingPoint!(FuncParamTypes[i])) {
                                    if (args[FuncParamNames[i]].type == JSONType.float_) {
                                        floatValues[$ - 1] = args[FuncParamNames[i]].float_;
                                    } else {
                                        return makeWarning!(i)();
                                    }
                                } else static if (is(FuncParamTypes[i] : bool)) {
                                    if (among(args[FuncParamNames[i]].type,
                                            JSONType.true_, JSONType.false_)) {
                                        boolValues[$ - 1] = args[FuncParamNames[i]].boolean;
                                    } else {
                                        return makeWarning!(i)();
                                    }
                                } else {
                                    static assert(0, "Unsupported parameter type "
                                            ~ FuncParamTypes[i].stringof
                                            ~ " in function call " ~ memberName);
                                }
                            }
                            enum callFunc = {
                                string[] p;
                                static foreach (i; 0 .. FuncParamNames.length) {
                                    static if (is(FuncParamTypes[i] : string)) {
                                        p ~= "strValues[" ~ i.to!string ~ "]";
                                    } else static if (isIntegral!(FuncParamTypes[i])) {
                                        p ~= "intValues[" ~ i.to!string ~ "]";
                                    } else static if (isFloatingPoint!(FuncParamTypes[i])) {
                                        p ~= "floatValues[" ~ i.to!string ~ "]";
                                    } else static if (is(FuncParamTypes[i] : bool)) {
                                        p ~= "boolValues[" ~ i.to!string ~ "]";
                                    } else {
                                        static assert(0, "Unsupported parameter type "
                                                ~ FuncParamTypes[i].stringof
                                                ~ " in function call " ~ memberName);
                                    }
                                }
                                return "return member(ctx, " ~ p.join(",") ~ ");";
                            }();
                            mixin(callFunc);
                        };
                        addFunction(FunctionDesc(name: memberName, desc: funcDesc,
                                params: params, callback: &funcCallback));
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

private:

shared(FunctionDesc[]) registeredFunc;
