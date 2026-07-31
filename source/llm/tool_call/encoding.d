module llm.tool_call.encoding;

import logger = std.logger;
import std.base64 : Base64;
import std.digest : toHexString;
import std.digest.md : md5Of;
import std.format : format;
import std.string : representation;

import llm.tool_call;

mixin RegisterLlmFunctions!();

struct Base64EncodeParams {
    @ParamDescription("Text to encode as Base64")
    string data;
}

@Function("Encode text as Base64. Return encoded or error")
ExecuteFuncResult base64Encode(Context baseCtx, Base64EncodeParams params) @safe {
    try {
        return ExecuteFuncResult(Base64.encode(cast(const(ubyte)[]) params.data), success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(format!"error: failed to encode: %s"(e.msg), success: false);
    }
}

struct Base64DecodeParams {
    @ParamDescription("Base64-encoded text to decode")
    string data;
}

@Function("Decode Base64 to data. Return decoded or error")
ExecuteFuncResult base64Decode(Context baseCtx, Base64DecodeParams params) @safe {
    try {
        return ExecuteFuncResult(cast(string) Base64.decode(params.data).idup, success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(format!"error: failed to decode: %s"(e.msg), success: false);
    }
}

struct Md5HashParams {
    @ParamDescription("Data to compute MD5 hash for")
    string data;
}

@Function("Calculate the MD5 hash of data. Returns a hexadecimal string.")
ExecuteFuncResult md5Hash(Context baseCtx, Md5HashParams params) @safe {
    try {
        return ExecuteFuncResult(params.data.representation.md5Of.toHexString.idup, success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(format!"error: %s"(e.msg), success: false);
    }
}
