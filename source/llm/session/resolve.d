/// Session reference resolution: maps a user-supplied argument (index, id, or
/// title) to a session id, shared by the CLI slash commands and future UI.
module llm.session.resolve;

import std.conv : to, ConvException;
import std.string : toLower;

import my.optional : Optional, none, some;

import llm.session.types : SessionId, SessionMeta;

/** Resolve a user-supplied reference to a session id.
 *
 * Precedence: integer string -> 1-based index into `sessions`;
 * exact id match; case-insensitive exact title match.
 *
 * Params:
 *   sessions = list of sessions (as returned by `SessionStore.list()`)
 *   arg      = user input (index, id, or title)
 *
 * Returns: matched session id, or none if not found
 */
Optional!SessionId resolveSessionRef(const SessionMeta[] sessions, string arg) @safe pure {
    // 1. Try integer index (1-based)
    try {
        auto idx = arg.to!size_t;
        if (idx > 0 && idx <= sessions.length) {
            return some(SessionId(sessions[idx - 1].id.get));
        }
        return none!SessionId();
    } catch (ConvException) {
        // Not an integer, continue to other strategies
    }

    // 2. Exact id match
    foreach (s; sessions) {
        if (s.id.get == arg) {
            return some(SessionId(s.id.get));
        }
    }

    // 3. Case-insensitive exact title match
    auto lowerArg = arg.toLower;
    foreach (s; sessions) {
        if (s.title.toLower == lowerArg) {
            return some(SessionId(s.id.get));
        }
    }

    return none!SessionId();
}
