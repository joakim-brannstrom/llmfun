module llm.tool_call.encoding;

import logger = std.logger;
import std.base64 : Base64;
import std.digest : toHexString;
import std.digest.md : md5Of;
import std.format : format;
import std.string : representation;

import llm.tool_call;

mixin RegisterLlmFunctions!();

@Function("Encode text as Base64. Return encoded or error")
ExecuteFuncResult base64Encode(Context ctx, string data) @safe {
    try {
        return ExecuteFuncResult(Base64.encode(cast(const(ubyte)[]) data), success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(format!"error: failed to encode: %s"(e.msg), success: false);
    }
}

@Function("Decode Base64 to data. Return decoded or error")
ExecuteFuncResult base64Decode(Context ctx, string data) @safe {
    try {
        return ExecuteFuncResult(cast(string) Base64.decode(data).idup, success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(format!"error: failed to decode: %s"(e.msg), success: false);
    }
}

@Function("Calculate the MD5 hash of data. Returns a hexadecimal string.")
ExecuteFuncResult md5Hash(Context ctx, string data) @safe {
    try {
        return ExecuteFuncResult(data.representation.md5Of.toHexString.idup, success: true);
    } catch (Exception e) {
        return ExecuteFuncResult(format!"error: %s"(e.msg), success: false);
    }
}
