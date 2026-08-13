/// Chat session data types and id helpers: `SessionMeta` describes the header
/// fields of a session file, `SessionFile` carries the full parsed document,
/// and the helpers generate and validate the D10 id format.
module llm.session.types;

import std.conv : text;
import std.datetime : Clock;
import std.json : JSONValue;
import std.random : uniform;
import std.regex : regex, Regex;
import std.string : format;

import my.named_type : Comparable, ForwardStringable, Lengthable, NamedType, Tag;

/// Maximum number of attempts (initial + retries) when a generated id collides (D10).
enum MaxIdRetries = 5;

/// Maximum length of the preview string extracted from the first user message.
enum PreviewMaxChars = 25;

/// Strong type for session ids: a session id is not interchangeable with
/// titles, previews, or other strings (type safety at the store boundary).
alias SessionId = NamedType!(string, Tag!"SessionId", null, Comparable,
        ForwardStringable, Lengthable);

/// Regex for valid session ids (D12): YYYYMMDD-HHMMSS-4hex.
private Regex!char IdPattern;

static this() {
    // calculate regex at runtime to avoid compile time bloat
    IdPattern = regex(r"^\d{8}-\d{6}-[0-9a-f]{4}$");
}

/** Session metadata stored in the header of a session file.
 *
 * Fields `messageCount`, `userMessageCount`, and `preview` are computed from
 * the messages array on load/list/save. The `extra` field preserves any header
 * keys that the store does not understand, enabling future extensions (D2).
 */
struct SessionMeta {
    SessionId id; // immutable; == filename (no extension)
    string title;
    long createdAt; // unix seconds
    long updatedAt; // unix seconds; bumped on every save
    size_t messageCount; // total entries in messages[]
    size_t userMessageCount; // entries with role == "user"
    string preview; // first string-content user message, truncated
    JSONValue extra; // unknown header keys, preserved on save (D2)
}

/** A loaded session file: metadata header plus the full JSON document. */
struct SessionFile {
    SessionMeta meta;
    JSONValue doc; // full file JSON: header keys + "messages"
}

/** Generate a new session id in the format YYYYMMDD-HHMMSS-NNNN (D10).
 *
 * The hex suffix is generated randomly. The `exists` delegate is called to
 * check for collisions; on collision the suffix is regenerated, bounded by
 * `MaxIdRetries` attempts in total (D10).
 *
 * Params:
 *   exists = delegate that returns true if the id already exists on disk
 *
 * Returns: a unique session id
 *
 * Throws: `Exception` if collision persists after all retries
 */
package SessionId generateId(bool delegate(SessionId) exists) {
    auto now = Clock.currTime();
    auto timePart = format("%04d%02d%02d-%02d%02d%02d", now.year, now.month,
            now.day, now.hour, now.minute, now.second);

    foreach (attempt; 0 .. MaxIdRetries) {
        auto randVal = uniform(0, 0xFFFF);
        auto suffix = format("%04x", randVal);
        auto id = SessionId(timePart ~ "-" ~ suffix);
        if (!exists(id)) {
            return id;
        }
    }

    throw new Exception(i"Failed to generate unique session id after $(MaxIdRetries) attempts".text);
}

/** Generate a default title from the current local date (YYYY-MM-DD). */
package string generateDateTitle() @safe {
    auto now = Clock.currTime();
    return format("%04d-%02d-%02d", now.year, now.month, now.day);
}

package bool isValidId(SessionId id) @safe {
    import std.regex : match;
    import std.range : empty;

    return !id.get.match(IdPattern).empty;
}

/// Known header keys preserved on save; unknown keys go into `meta.extra` (D2).
package immutable(string[]) KnownHeaderKeys = [
    "id", "title", "createdAt", "updatedAt", "messageCount",
    "userMessageCount", "preview", "messages"
];
