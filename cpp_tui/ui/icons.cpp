/// The glyph table (ui/icons.h): the two columns of data, and the probes that decide
/// which one a surface gets.
///
/// The data is one row per `Icon` in declaration order. `test_icons` walks it and
/// checks the properties the rest of the shell relies on - one cell, one codepoint,
/// printable ASCII fallback, the two forms differ, no emoji - so a row edited by hand
/// is measured before it is shipped rather than after a layout moved. The comment on
/// each row names the codepoint, because "▸" and "▾" are one keystroke apart in a
/// diff.
#include "icons.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace llmfun::tui::ui {
namespace {

struct GlyphRow {
    const char* glyph; ///< the Unicode form: exactly one cell, one codepoint
    const char* ascii; ///< the fallback: printable ASCII
};

// Deliberately *unsized*: the row count is the initializer's, so the static_assert
// below compares what the table actually holds against the enum. Sizing the array
// `kIconCount` instead would make the assert a tautology and let a missing row
// value-initialize to {nullptr, nullptr} - a null glyph handed out by a public
// accessor (mutation-tested: deleting a row must fail the build, not the caller).
constexpr GlyphRow kGlyphs[] = {
    {"▸", ">"},   // Rest              U+25B8 BLACK RIGHT-POINTING SMALL TRIANGLE
    {"●", "o"},   // Active            U+25CF BLACK CIRCLE
    {"◐", "*"},   // Running           U+25D0 CIRCLE WITH LEFT HALF BLACK
    {"•", "."},   // Unread            U+2022 BULLET
    {"○", "-"},   // Pending           U+25CB WHITE CIRCLE
    {"✓", "+"},   // Ok                U+2713 CHECK MARK
    {"▲", "!"},   // Warn              U+25B2 BLACK UP-POINTING TRIANGLE
    {"✕", "x"},   // Error             U+2715 MULTIPLICATION X
    {"◇", "i"},   // Info              U+25C7 WHITE DIAMOND
    {"◐", "*"},   // Spinner           U+25D0 CIRCLE WITH LEFT HALF BLACK
    {"▾", "v"},   // DisclosureOpen    U+25BE BLACK DOWN-POINTING SMALL TRIANGLE
    {"▸", ">"},   // DisclosureClosed  U+25B8 BLACK RIGHT-POINTING SMALL TRIANGLE
    {"▌", "|"},   // Rule              U+258C LEFT HALF BLOCK
    {"…", "..."}, // Ellipsis          U+2026 HORIZONTAL ELLIPSIS
};

static_assert(sizeof(kGlyphs) / sizeof(kGlyphs[0]) == static_cast<std::size_t>(kIconCount),
              "kGlyphs must have one row per Icon, in declaration order");

/// A row for an icon inside the table, `Rest` for one that is not. A bad cast then
/// misdraws a mark instead of reading past the table (the same trade `tokenIndex()`
/// makes for a colour).
const GlyphRow& row(Icon which) {
    const int index = static_cast<int>(which);
    return kGlyphs[index >= 0 && index < kIconCount ? index : 0];
}

const char* envValue(EnvLookup lookup, const char* name) {
    return lookup != nullptr ? lookup(name) : std::getenv(name);
}

char toLower(char value) {
    return (value >= 'A' && value <= 'Z') ? char(value - 'A' + 'a') : value;
}

bool startsWithIgnoreCase(const char* value, const char* prefix) {
    if (value == nullptr || prefix == nullptr)
        return false;
    std::size_t i = 0;
    for (; prefix[i] != '\0'; ++i) {
        if (value[i] == '\0' || toLower(value[i]) != toLower(prefix[i]))
            return false;
    }
    return true;
}

/// A locale name whose language is Chinese, Japanese or Korean: `zh_CN.UTF-8`,
/// `ja_JP.eucJP`, `ko_KR`, `zh`. Only the language field is read - the codeset and
/// modifier cannot change how wide an ambiguous glyph is drawn - and the field has to
/// end there, so `zhx` is not Chinese and neither is `jakarta`.
bool isCjkLocale(const char* locale) {
    if (locale == nullptr || locale[0] == '\0' || locale[1] == '\0')
        return false;
    const char first = toLower(locale[0]);
    const char second = toLower(locale[1]);
    const bool cjk = (first == 'z' && second == 'h') || (first == 'j' && second == 'a') ||
                     (first == 'k' && second == 'o');
    if (!cjk)
        return false;
    const char after = locale[2];
    return after == '\0' || after == '_' || after == '-' || after == '.' || after == '@';
}

/// The locale that decides the character type, by the POSIX cascade: `LC_ALL`
/// overrides `LC_CTYPE`, which overrides `LANG`. An empty value is unset - the
/// shell's own convention - so `LC_CTYPE=` in a profile does not hide `LANG`.
const char* characterLocale(EnvLookup lookup) {
    static const char* const kCharacterTypeVars[] = {"LC_ALL", "LC_CTYPE", "LANG"};
    for (const char* name : kCharacterTypeVars) {
        const char* value = envValue(lookup, name);
        if (value != nullptr && *value != '\0')
            return value;
    }
    return nullptr;
}

} // namespace

const char* glyph(Icon which) { return row(which).glyph; }

const char* asciiFallback(Icon which) { return row(which).ascii; }

const char* icon(Icon which, GlyphSet set) {
    return set == GlyphSet::Ascii ? row(which).ascii : row(which).glyph;
}

const char* icon(Icon which) { return icon(which, textGlyphSet()); }

bool useAscii(EnvLookup lookup) {
    // 1. The explicit switch, both ways. The design names the "=1" side; "=0" is the
    //    escape hatch for a CJK locale whose terminal draws the ambiguous glyphs
    //    narrow, and it has to win over the heuristics below or there is no way to
    //    say "I know my terminal, use the glyphs".
    const char* forced = envValue(lookup, "LLMFUN_ASCII");
    if (forced != nullptr) {
        if (std::strcmp(forced, "1") == 0)
            return true;
        if (std::strcmp(forced, "0") == 0)
            return false;
    }
    // 2. The Linux console: its font carries the ACS line-drawing set and little
    //    else, so every mark here would paint as "?". Matched by prefix because the
    //    console's terminfo entries are `linux`, `linux-16color`, ...
    if (startsWithIgnoreCase(envValue(lookup, "TERM"), "linux"))
        return true;
    // 3. A CJK locale, where the ambiguous-width characters used below are commonly
    //    drawn two cells wide.
    return isCjkLocale(characterLocale(lookup));
}

bool useAscii() { return useAscii(nullptr); }

GlyphSet textGlyphSet() { return useAscii() ? GlyphSet::Ascii : GlyphSet::Glyphs; }

GlyphSet pixelGlyphSet() {
    // The GUI draws through the ImGui font atlas, and the shipped atlas cannot show
    // the marks: the embedded ProggyClean bitmap carries none of the 12 distinct
    // marks, and Roboto-Medium (reachable as LLMFUN_GUI_FONT, the design's UI font)
    // carries two. A codepoint the atlas lacks is not drawn as the mark there - ImGui
    // substitutes the font's fallback - so a GUI view using the glyph column would
    // paint a row of "?".
    //
    // This stays a decision, not a probe: the atlas lives behind ImGui and this module
    // may not include it. `test_icons` measures both fonts and asserts this function
    // *equals* that measurement, so the day task 20 merges a symbol-capable font the
    // run fails and the flip is deliberate.
    return GlyphSet::Ascii;
}

} // namespace llmfun::tui::ui
