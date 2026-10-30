// test_icons.cpp
//
// Unit test for the glyph table (task 15, design §4.8 / §4.15 "Icons"): one
// character per meaning, with a single-cell Unicode form and a plain ASCII
// fallback chosen automatically for terminals that cannot render the mark.
//
// What is pinned here, case by case:
//
//   1. the table      one row per `Icon` in declaration order, every glyph one
//                     cell and one codepoint, every fallback printable ASCII and
//                     one cell (the cut marker is the documented three), the two
//                     forms differ for every icon, and no glyph is an emoji. The
//                     emoji exclusion is measured rather than listed: the run
//                     clears `LLMFUN_IMTUI_EMOJI_WIDTH`, so an emoji-presentation
//                     codepoint measures two cells and fails the one-cell rule;
//
//   1b. the cut marker
//                     the one fallback that is not one cell is the string
//                     `ui::truncate()` is handed: every budget from one cell up
//                     still produces a cut that fits, in both modes;
//
//   2. detection      `LLMFUN_ASCII` (1 / 0 / empty / junk), `TERM=linux*` and a
//                     CJK `LC_ALL`/`LC_CTYPE`/`LANG`, against an injected
//                     environment, including the POSIX cascade (LC_ALL wins, an
//                     empty value is unset) and the `=0` escape hatch;
//
//   3. the accessor   `icon()` through the real process environment, both ways:
//                     it is the one function a terminal path calls, so it is the
//                     one that has to answer the fallback when the terminal needs
//                     it;
//
//   3b. the set       `GlyphSet` as a value: each column addressed directly, the
//                     text grid's set following the terminal, and the *pixel*
//                     profile's set checked against what the GUI's font atlases
//                     actually carry (the embedded bitmap font: 0 of the 12 marks,
//                     Roboto-Medium: 2) - with ImGui's substitution measured too,
//                     because a mark the atlas lacks is not drawn as the mark. The
//                     check is an equivalence, so the constant cannot go stale in
//                     either direction without failing the run;
//
//   4. the screen     the whole table laid out as a legend and rendered through
//                     the real ImTui text backend at 80x24 and 40x15, in both
//                     sets (the legend is built with `icon(Icon, GlyphSet)`, so the
//                     render half never touches the environment). Cell by cell: the
//                     row is exactly the expected one (no "?" substitution, no
//                     U+FFFD, no drift), each icon occupies exactly one cell *as the
//                     renderer measures it* (`chwidth` 1, no continuation codepoint),
//                     and the word is read back out of the grid one blank cell after
//                     the mark instead of being compared with the string the same
//                     builder produced. Across the two sets the label moves only by
//                     the width difference between the two forms - which is the whole
//                     point of the fallback: the alignment survives.
//
// Documented deviations, each measured rather than assumed:
//   * `Ellipsis`'s fallback is "...", three cells, because a cut marker is a
//     string and `ui::truncate()` measures the marker it is handed (its default
//     marker *is* `ui::icon(Icon::Ellipsis)`). It is the one icon whose fallback is
//     not one cell, and it is asserted instead of assumed;
//   * `Rest`/`DisclosureClosed` and `Running`/`Spinner` share a form by design
//     (see ui/icons.h); the pairs are asserted so a future divergence is a
//     decision and not an accident;
//   * the pixel profile draws the ASCII column (`pixelGlyphSet()`) because its font
//     atlas cannot draw the marks; the equivalence check in 3b is what makes that a
//     measurement rather than an opinion.
//
// Layering: the binary compiles ui/icons.cpp and ui/text.cpp itself, so the glyph
// module is built the way a consumer builds it - no ImGui, no tui.h, no v4 type
// reaches it - while the legend half links the app library for the real text
// backend and nothing else.
//
// Exit codes: 0 = every check passed; 1 = at least one failed; 2 = usage error.
// Run from the build dir: ./test_icons [-v] [--dump DIR]

#include "ui/icons.h"
#include "ui/text.h"

#include "imtui/imtui.h" // imgui + ImTui_ImplText_Init/NewFrame/RenderDrawData

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <vector>

namespace ui = llmfun::tui::ui;

namespace {

int g_failures = 0;
bool g_verbose = false;

/// The icon names, in declaration order - the words the legend and the failure
/// messages use. Left unsized on purpose: the assert then compares the *initializer*
/// count with the enum, so an icon added without a name is a build error rather than
/// a legend row (or a failure message) built from a null string.
constexpr const char* kNames[] = {
    "Rest",  "Active", "Running", "Unread",   "Pending",   "Ok",   "Warn",
    "Error", "Info",   "Spinner", "Expanded", "Collapsed", "Rule", "Ellipsis",
};
static_assert(sizeof(kNames) / sizeof(kNames[0]) == static_cast<std::size_t>(ui::kIconCount),
              "kNames must name every Icon, in declaration order");

const char* nameOf(int index) {
    return index >= 0 && index < ui::kIconCount ? kNames[index] : "<invalid>";
}

void check(bool condition, const std::string& what) {
    if (condition) {
        if (g_verbose)
            std::fprintf(stderr, "ok   %s\n", what.c_str());
        return;
    }
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++g_failures;
}

/// Text as it will read in a failure message: quotes around it, control bytes
/// escaped, a UTF-8 glyph left alone so "▲" is recognisable as itself.
std::string describe(std::string_view text) {
    std::string out = "\"";
    for (const char raw : text) {
        const unsigned char byte = static_cast<unsigned char>(raw);
        if (byte == '\\' || byte == '"') {
            out += '\\';
            out += raw;
        } else if (byte < 0x20 || byte == 0x7F) {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\x%02X", byte);
            out += escaped;
        } else {
            out += raw;
        }
    }
    out += '"';
    return out;
}

std::string hexCodepoint(uint32_t cp) {
    char text[16];
    std::snprintf(text, sizeof(text), "U+%04X", cp);
    return text;
}

/// Codepoints in a well-formed UTF-8 string. The table is ours, so a malformed
/// sequence is a failure of the table, not something to recover from: a stray
/// byte counts as one codepoint and is reported by the caller's width check.
std::vector<uint32_t> codepoints(std::string_view utf8) {
    std::vector<uint32_t> out;
    std::size_t at = 0;
    while (at < utf8.size()) {
        const unsigned char lead = static_cast<unsigned char>(utf8[at]);
        std::size_t length = 1;
        uint32_t value = lead;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
            value = lead & 0x1Fu;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            value = lead & 0x0Fu;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            value = lead & 0x07u;
        }
        if (at + length > utf8.size()) {
            out.push_back(0xFFFD);
            break;
        }
        for (std::size_t i = 1; i < length; ++i) {
            const unsigned char continuation = static_cast<unsigned char>(utf8[at + i]);
            if ((continuation & 0xC0) != 0x80) {
                out.push_back(0xFFFD);
                break;
            }
            value = (value << 6) | (continuation & 0x3Fu);
        }
        out.push_back(value);
        at += length;
    }
    return out;
}

std::string utf8Of(uint32_t cp) {
    std::string s;
    if (cp < 0x80) {
        s.push_back(char(cp));
    } else if (cp < 0x800) {
        s.push_back(char(0xC0 | (cp >> 6)));
        s.push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        s.push_back(char(0xE0 | (cp >> 12)));
        s.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(char(0x80 | (cp & 0x3F)));
    } else {
        s.push_back(char(0xF0 | (cp >> 18)));
        s.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
        s.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(char(0x80 | (cp & 0x3F)));
    }
    return s;
}

// ---------------------------------------------------------------------------
// Environment fixtures
// ---------------------------------------------------------------------------

/// One environment variable, saved so a check can put the process back the way it
/// found it: the run must not leak a TERM or a locale into what it prints.
struct SavedEnv {
    std::string name;
    bool set = false;
    std::string value;

    explicit SavedEnv(const char* variable) : name(variable) {
        const char* current = std::getenv(variable);
        set = current != nullptr;
        if (set)
            value = current;
    }

    void restore() const {
        if (set)
            ::setenv(name.c_str(), value.c_str(), 1);
        else
            ::unsetenv(name.c_str());
    }
};

// ---------------------------------------------------------------------------
// 1. The table
// ---------------------------------------------------------------------------

void checkTable() {
    check(ui::kIconCount == 14, "the table has the 14 icons the contract lists");

    for (int i = 0; i < ui::kIconCount; ++i) {
        const ui::Icon which = static_cast<ui::Icon>(i);
        const std::string name = std::string("icon ") + nameOf(i);

        const char* glyph = ui::glyph(which);
        const char* fallback = ui::asciiFallback(which);
        check(glyph != nullptr && glyph[0] != '\0', name + ": glyph() is empty");
        check(fallback != nullptr && fallback[0] != '\0', name + ": asciiFallback() is empty");
        if (glyph == nullptr || fallback == nullptr)
            continue;

        // One cell, one codepoint: a two-codepoint sequence could measure one
        // cell (a base plus a combining mark) and still be drawn as two glyphs by
        // a terminal that does not compose them.
        const int cells = ui::displayWidth(glyph);
        check(cells == 1, name + ": glyph " + describe(glyph) + " measures " +
                              std::to_string(cells) + " cells, expected 1");
        const std::vector<uint32_t> parts = codepoints(glyph);
        check(parts.size() == 1, name + ": glyph " + describe(glyph) + " is " +
                                     std::to_string(parts.size()) + " codepoints, expected 1");

        // The fallback is a plain printable ASCII string: a control byte or a
        // non-ASCII glyph in the fallback would defeat the reason it exists.
        bool printable = true;
        for (const char c : std::string(fallback)) {
            const unsigned char byte = static_cast<unsigned char>(c);
            if (byte < 0x20 || byte > 0x7E)
                printable = false;
        }
        check(printable, name + ": fallback " + describe(fallback) + " is printable ASCII");

        const int fallbackCells = ui::displayWidth(fallback);
        if (which == ui::Icon::Ellipsis) {
            // The documented exception: a cut marker handed to ui::truncate(),
            // which measures the marker it is given.
            check(fallbackCells == 3,
                  name + ": fallback " + describe(fallback) + " is three cells (a cut marker)");
        } else {
            check(fallbackCells == 1, name + ": fallback " + describe(fallback) + " measures " +
                                          std::to_string(fallbackCells) + " cells, expected 1");
        }

        check(std::strcmp(glyph, fallback) != 0, name + ": the two forms differ (" +
                                                     describe(glyph) + " vs " + describe(fallback) +
                                                     ")");
    }

    // No emoji anywhere. The renderer's default mode is the strict one - an
    // emoji-presentation codepoint measures two cells there - and the run cleared
    // LLMFUN_IMTUI_EMOJI_WIDTH at start-up, so this check states that the one-cell
    // rule above is the emoji rule as well. A known emoji proves the mode.
    check(ui::displayWidth("✅") == 2,
          "the run measures emoji in the renderer's default mode (U+2705 is two cells), so a "
          "two-cell glyph cannot pass the table checks above");

    // A bad cast answers the first row: a caller with a broken enum value misdraws
    // a mark, it does not read past the table.
    check(std::strcmp(ui::glyph(static_cast<ui::Icon>(99)), ui::glyph(ui::Icon::Rest)) == 0,
          "an icon past the table answers the first row");
    check(std::strcmp(ui::asciiFallback(static_cast<ui::Icon>(-1)),
                      ui::asciiFallback(ui::Icon::Rest)) == 0,
          "a negative icon answers the first row");

    // The documented shared forms: two meanings that "both open the row" and a
    // second name for the reduced-motion busy mark. Asserted so a divergence is a
    // decision (and a doc update) rather than a silent screen change.
    check(std::strcmp(ui::glyph(ui::Icon::Rest), ui::glyph(ui::Icon::DisclosureClosed)) == 0 &&
              std::strcmp(ui::asciiFallback(ui::Icon::Rest),
                          ui::asciiFallback(ui::Icon::DisclosureClosed)) == 0,
          "Rest and DisclosureClosed share the 'this row opens' mark in both modes");
    check(std::strcmp(ui::glyph(ui::Icon::Spinner), ui::glyph(ui::Icon::Running)) == 0 &&
              std::strcmp(ui::asciiFallback(ui::Icon::Spinner),
                          ui::asciiFallback(ui::Icon::Running)) == 0,
          "Spinner is Running's mark (the reduced-motion form of the same state)");

    // The set itself, spelled out: a reader of the golden screens and this table
    // are looking at the same characters, and a swap ("▸" for "▾") is one
    // keystroke away in a diff.
    struct Pair {
        ui::Icon icon;
        const char* glyph;
        const char* ascii;
    };
    const Pair expected[] = {
        {ui::Icon::Rest, "▸", ">"},           {ui::Icon::Active, "●", "o"},
        {ui::Icon::Running, "◐", "*"},        {ui::Icon::Unread, "•", "."},
        {ui::Icon::Pending, "○", "-"},        {ui::Icon::Ok, "✓", "+"},
        {ui::Icon::Warn, "▲", "!"},           {ui::Icon::Error, "✕", "x"},
        {ui::Icon::Info, "◇", "i"},           {ui::Icon::Spinner, "◐", "*"},
        {ui::Icon::DisclosureOpen, "▾", "v"}, {ui::Icon::DisclosureClosed, "▸", ">"},
        {ui::Icon::Rule, "▌", "|"},           {ui::Icon::Ellipsis, "…", "..."},
    };
    check(sizeof(expected) / sizeof(expected[0]) == size_t(ui::kIconCount),
          "the expected set names every icon");
    for (const Pair& pair : expected) {
        const std::string name = std::string("the set: ") + nameOf(static_cast<int>(pair.icon));
        // Guarded even though the row count is a build-time assert: a null here would
        // abort inside strcmp/describe instead of printing the one readable line that
        // says which row went missing.
        const char* const actualGlyph = ui::glyph(pair.icon);
        const char* const actualAscii = ui::asciiFallback(pair.icon);
        check(actualGlyph != nullptr && std::strcmp(actualGlyph, pair.glyph) == 0,
              name + " is " + describe(pair.glyph) + " (the table says " +
                  describe(actualGlyph != nullptr ? actualGlyph : "<null>") + ")");
        check(actualAscii != nullptr && std::strcmp(actualAscii, pair.ascii) == 0,
              name + " falls back to " + describe(pair.ascii) + " (the table says " +
                  describe(actualAscii != nullptr ? actualAscii : "<null>") + ")");
    }
}

// ---------------------------------------------------------------------------
// 1b. The cut marker, which is the one fallback that is not one cell
// ---------------------------------------------------------------------------

/// `Ellipsis` is not a column mark but a cut marker, and the module that consumes
/// it is `ui::truncate()`, which measures the marker it is handed. In ASCII mode that
/// marker is three dots instead of one, so this pins the contract that makes the wider
/// fallback acceptable in the first place: a cut still never overflows the budget it
/// exists to respect - and the two-argument `truncate()` is the call every label in
/// tasks 16/25/26 will make, so its *default* has to follow the same decision.
void checkCutMarker() {
    const SavedEnv ascii("LLMFUN_ASCII");
    const bool modes[2] = {false, true};
    for (const bool asciiMode : modes) {
        ::setenv("LLMFUN_ASCII", asciiMode ? "1" : "0", 1);
        const char* const marker = ui::icon(ui::Icon::Ellipsis);
        const std::string label =
            std::string("the cut marker in ") + (asciiMode ? "ASCII" : "Unicode") + " mode";
        check(marker != nullptr && std::strcmp(marker, asciiMode ? "..." : "…") == 0,
              label + " is " + describe(asciiMode ? "..." : "…") + " (got " +
                  describe(marker != nullptr ? marker : "<null>") + ")");
        if (marker == nullptr)
            continue;

        // Every budget from "one cell" (where the marker itself has to be cut)
        // upwards: the result never exceeds the budget, whichever way the marker was
        // passed - explicitly, or taken from the table by the two-argument form.
        for (int cells = 1; cells <= 12; ++cells) {
            const std::string explicitCut = ui::truncate("abcdefghijkl", cells, marker);
            const int explicitUsed = ui::displayWidth(explicitCut);
            check(explicitUsed <= cells, label + ": truncate to " + std::to_string(cells) +
                                             " cells stays inside the budget (" +
                                             describe(explicitCut) + " is " +
                                             std::to_string(explicitUsed) + ")");
            const std::string defaultCut = ui::truncate("abcdefghijkl", cells);
            check(defaultCut == explicitCut, label + ": truncate(text, " + std::to_string(cells) +
                                                 ") is the explicit-marker cut (got " +
                                                 describe(defaultCut) + ", expected " +
                                                 describe(explicitCut) + ")");
        }
    }
    ascii.restore();
}

// ---------------------------------------------------------------------------
// 2. Detection, against an injected environment
// ---------------------------------------------------------------------------

using EnvMap = std::map<std::string, std::string>;

const EnvMap* g_env = nullptr;

const char* testEnv(const char* name) {
    if (g_env == nullptr)
        return nullptr;
    const auto found = g_env->find(name);
    return found == g_env->end() ? nullptr : found->second.c_str();
}

void checkDetection() {
    struct Case {
        const char* what;
        EnvMap env;
        bool expected;
    };
    const std::vector<Case> cases = {
        // The baseline: a busy terminal environment asks for the glyphs.
        {"an empty environment keeps the glyphs", {}, false},
        {"TERM=xterm-256color keeps the glyphs", {{"TERM", "xterm-256color"}}, false},
        {"TERM=screen-256color keeps the glyphs", {{"TERM", "screen-256color"}}, false},

        // The Linux console: no glyphs beyond the ACS set.
        {"TERM=linux forces ASCII", {{"TERM", "linux"}}, true},
        {"TERM=linux is matched case-insensitively", {{"TERM", "Linux"}}, true},
        {"TERM=linux-16color forces ASCII", {{"TERM", "linux-16color"}}, true},
        {"TERM=xlinux does not force ASCII", {{"TERM", "xlinux"}}, false},

        // A CJK locale, where the ambiguous-width marks are drawn double.
        {"LC_CTYPE=ja_JP.UTF-8 forces ASCII", {{"LC_CTYPE", "ja_JP.UTF-8"}}, true},
        {"LC_CTYPE=zh_CN.UTF-8 forces ASCII", {{"LC_CTYPE", "zh_CN.UTF-8"}}, true},
        {"LC_CTYPE=ko_KR.eucKR forces ASCII", {{"LC_CTYPE", "ko_KR.eucKR"}}, true},
        {"a bare zh locale forces ASCII", {{"LC_CTYPE", "zh"}}, true},
        {"LC_CTYPE=en_US.UTF-8 keeps the glyphs", {{"LC_CTYPE", "en_US.UTF-8"}}, false},
        {"LC_CTYPE=C keeps the glyphs", {{"LC_CTYPE", "C"}}, false},
        {"LC_CTYPE=POSIX keeps the glyphs", {{"LC_CTYPE", "POSIX"}}, false},
        {"a language that only starts like a CJK one keeps the glyphs",
         {{"LC_CTYPE", "jakarta"}},
         false},

        // The POSIX cascade: LC_ALL wins over LC_CTYPE, LC_CTYPE over LANG, and an
        // empty value is unset.
        {"LC_ALL wins over LC_CTYPE",
         {{"LC_ALL", "ja_JP.UTF-8"}, {"LC_CTYPE", "en_US.UTF-8"}},
         true},
        {"LC_ALL=C wins over a CJK LC_CTYPE",
         {{"LC_ALL", "C"}, {"LC_CTYPE", "ja_JP.UTF-8"}},
         false},
        {"LC_CTYPE wins over LANG", {{"LC_CTYPE", "en_US.UTF-8"}, {"LANG", "ja_JP.UTF-8"}}, false},
        {"an empty LC_CTYPE falls through to LANG",
         {{"LC_CTYPE", ""}, {"LANG", "ko_KR.UTF-8"}},
         true},
        {"LANG=ja_JP.UTF-8 forces ASCII", {{"LANG", "ja_JP.UTF-8"}}, true},

        // The explicit switch, both ways.
        {"LLMFUN_ASCII=1 forces ASCII", {{"LLMFUN_ASCII", "1"}}, true},
        {"LLMFUN_ASCII=1 wins over TERM",
         {{"LLMFUN_ASCII", "1"}, {"TERM", "xterm-256color"}},
         true},
        {"LLMFUN_ASCII=0 wins over TERM=linux", {{"LLMFUN_ASCII", "0"}, {"TERM", "linux"}}, false},
        {"LLMFUN_ASCII=0 wins over a CJK locale",
         {{"LLMFUN_ASCII", "0"}, {"LC_CTYPE", "ja_JP.UTF-8"}},
         false},
        {"an empty LLMFUN_ASCII means unset", {{"LLMFUN_ASCII", ""}, {"TERM", "linux"}}, true},
        {"a junk LLMFUN_ASCII is ignored", {{"LLMFUN_ASCII", "yes"}, {"TERM", "linux"}}, true},
    };

    for (const Case& c : cases) {
        g_env = &c.env;
        const bool got = ui::useAscii(testEnv);
        g_env = nullptr;
        check(got == c.expected, std::string("detection: ") + c.what + " (got " +
                                     (got ? "ASCII" : "glyphs") + ", expected " +
                                     (c.expected ? "ASCII" : "glyphs") + ")");
    }

    // The lookup-less overload must agree with the injected one for the same
    // environment; that is the only way the two can disagree about a real run.
    check(ui::useAscii() == ui::useAscii(nullptr),
          "useAscii() and useAscii(nullptr) read the same environment");
}

// ---------------------------------------------------------------------------
// 3. The accessor, through the real environment
// ---------------------------------------------------------------------------

void checkAccessor() {
    const SavedEnv ascii("LLMFUN_ASCII");
    const SavedEnv term("TERM");
    const SavedEnv lcAll("LC_ALL");
    const SavedEnv lcCtype("LC_CTYPE");
    const SavedEnv lang("LANG");

    // Glyphs: an explicit "no fallback" over a CJK locale.
    ::setenv("LLMFUN_ASCII", "0", 1);
    check(!ui::useAscii(), "with LLMFUN_ASCII=0 the accessor asks for the glyphs");
    for (int i = 0; i < ui::kIconCount; ++i) {
        const ui::Icon which = static_cast<ui::Icon>(i);
        check(std::strcmp(ui::icon(which), ui::glyph(which)) == 0,
              std::string("icon(") + nameOf(i) + ") is the glyph when the glyphs are asked for");
    }

    // Fallback: the forced switch.
    ::setenv("LLMFUN_ASCII", "1", 1);
    check(ui::useAscii(), "with LLMFUN_ASCII=1 the accessor asks for the fallback");
    for (int i = 0; i < ui::kIconCount; ++i) {
        const ui::Icon which = static_cast<ui::Icon>(i);
        check(std::strcmp(ui::icon(which), ui::asciiFallback(which)) == 0,
              std::string("icon(") + nameOf(i) + ") is the fallback when ASCII is forced");
    }

    // Fallback: the console, with no explicit switch.
    ::unsetenv("LLMFUN_ASCII");
    ::unsetenv("LC_ALL");
    ::unsetenv("LC_CTYPE");
    ::unsetenv("LANG");
    ::setenv("TERM", "linux", 1);
    check(std::strcmp(ui::icon(ui::Icon::Ok), "+") == 0,
          "an unswitched Linux console draws the Ok fallback");
    ::setenv("TERM", "xterm-256color", 1);
    check(std::strcmp(ui::icon(ui::Icon::Ok), "✓") == 0,
          "an unswitched colour terminal draws the Ok glyph");

    ascii.restore();
    term.restore();
    lcAll.restore();
    lcCtype.restore();
    lang.restore();
}

// ---------------------------------------------------------------------------
// 3b. The surface's set, and what the GUI's font atlas can actually draw
// ---------------------------------------------------------------------------

/// The distinct codepoints of the table: `Rest`/`DisclosureClosed` share one mark and
/// so do `Running`/`Spinner`, which is 12 of the 14 rows.
std::vector<uint32_t> distinctMarks() {
    std::vector<uint32_t> marks;
    for (int i = 0; i < ui::kIconCount; ++i) {
        const std::vector<uint32_t> parts = codepoints(ui::glyph(static_cast<ui::Icon>(i)));
        if (parts.empty())
            continue;
        bool seen = false;
        for (const uint32_t known : marks)
            seen = seen || known == parts.front();
        if (!seen)
            marks.push_back(parts.front());
    }
    return marks;
}

/// How many of `marks` a font carries, or -1 when the font could not be baked.
int countMarksInFont(ImFont* font, float size, const std::vector<uint32_t>& marks) {
    ImFontBaked* baked = font != nullptr ? font->GetFontBaked(size) : nullptr;
    if (baked == nullptr)
        return -1;
    int present = 0;
    for (const uint32_t cp : marks) {
        if (baked->FindGlyphNoFallback(static_cast<ImWchar>(cp)) != nullptr)
            ++present;
    }
    return present;
}

/// `GlyphSet` is how a surface states what it can draw, and `pixelGlyphSet()` is the
/// GUI's answer. That answer is only honest if it matches the atlas, so this measures
/// the atlases the GUI can be running: the embedded bitmap font it ships by default
/// and, when the vendored copy is reachable, the design's UI font (Roboto-Medium,
/// reachable as `LLMFUN_GUI_FONT`). ImGui substitutes a codepoint the atlas lacks with
/// the font's fallback glyph - so a mark the atlas does not carry is *not* drawn as the
/// mark - which is why the pixel profile may not use the glyph column yet.
///
/// The check is an equivalence, not a direction: it fails when a font gains the marks
/// (the constant is then wrong and task 20 flips it) and when one loses them.
void checkGlyphSets() {
    // (1) The two columns, addressed directly: no environment involved.
    for (int i = 0; i < ui::kIconCount; ++i) {
        const ui::Icon which = static_cast<ui::Icon>(i);
        const std::string name = std::string("icon ") + nameOf(i);
        check(std::strcmp(ui::icon(which, ui::GlyphSet::Glyphs), ui::glyph(which)) == 0,
              name + ": GlyphSet::Glyphs is the Unicode form");
        check(std::strcmp(ui::icon(which, ui::GlyphSet::Ascii), ui::asciiFallback(which)) == 0,
              name + ": GlyphSet::Ascii is the fallback");
    }

    // (2) The text grid's set is the terminal's answer, whichever way the environment
    //     says it - and the one-argument accessor is that set.
    const SavedEnv ascii("LLMFUN_ASCII");
    const bool modes[2] = {false, true};
    for (const bool asciiMode : modes) {
        ::setenv("LLMFUN_ASCII", asciiMode ? "1" : "0", 1);
        const ui::GlyphSet expected = asciiMode ? ui::GlyphSet::Ascii : ui::GlyphSet::Glyphs;
        const std::string label = std::string("with LLMFUN_ASCII=") + (asciiMode ? "1" : "0");
        check(ui::textGlyphSet() == expected, label + ": the text grid's set follows the terminal");
        for (int i = 0; i < ui::kIconCount; ++i) {
            const ui::Icon which = static_cast<ui::Icon>(i);
            check(std::strcmp(ui::icon(which), ui::icon(which, ui::textGlyphSet())) == 0,
                  label + ": icon(" + nameOf(i) + ") is icon(..., textGlyphSet())");
        }
    }
    ascii.restore();

    // (3) The atlas measurement, and the pixel set that has to agree with it.
    const std::vector<uint32_t> marks = distinctMarks();
    check(marks.size() == 12,
          "the table has 12 distinct marks (" + std::to_string(marks.size()) + ")");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    int bitmapPresent = -1;
    unsigned bitmapFallback = 0;
    {
        // The embedded font the GUI ships by default (tui_backend_gui.cpp's pin).
        ImFontConfig config;
        config.SizePixels = 13.0f;
        ImFont* bitmap = ImGui::GetIO().Fonts->AddFontDefaultBitmap(&config);
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        bitmapPresent = countMarksInFont(bitmap, 13.0f, marks);
        bitmapFallback = bitmap != nullptr ? unsigned(bitmap->FallbackChar) : 0;
    }
    ImGui::DestroyContext();

    check(bitmapPresent >= 0, "the shipped GUI font bakes (measured " +
                                  std::to_string(bitmapPresent) + " of 12 marks)");
    check(bitmapPresent < int(marks.size()),
          "the shipped GUI font does not carry the marks (" + std::to_string(bitmapPresent) +
              " of 12) - if it does now, pixelGlyphSet() has to be flipped");
    bool fallbackIsAMark = false;
    for (const uint32_t cp : marks)
        fallbackIsAMark = fallbackIsAMark || cp == bitmapFallback;
    check(bitmapFallback != 0 && !fallbackIsAMark,
          "a mark the shipped GUI font lacks is substituted with " + hexCodepoint(bitmapFallback) +
              ", not drawn as the mark");

    // The design's UI font, when the vendored copy is next to the build. Not an
    // assumption: the file is only used to *measure*, and a missing file leaves the
    // bitmap answer standing.
    int filePresent = -1;
#ifdef TEST_ICONS_VENDOR_FONTS
    const std::string ttf = std::string(TEST_ICONS_VENDOR_FONTS) + "/Roboto-Medium.ttf";
    if (std::FILE* probe = std::fopen(ttf.c_str(), "rb")) {
        std::fclose(probe);
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImFontConfig config;
        config.SizePixels = 13.0f;
        ImFont* font = ImGui::GetIO().Fonts->AddFontFromFileTTF(ttf.c_str(), 13.0f, &config);
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        filePresent = countMarksInFont(font, 13.0f, marks);
        ImGui::DestroyContext();
        check(filePresent >= 0, "Roboto-Medium bakes (measured " + std::to_string(filePresent) +
                                    " of " + std::to_string(marks.size()) + " marks)");
    } else if (g_verbose) {
        std::fprintf(stderr,
                     "note: %s not found; the pixel set is checked against the bitmap font only\n",
                     ttf.c_str());
    }
#endif

    const int best = filePresent > bitmapPresent ? filePresent : bitmapPresent;
    const ui::GlyphSet expected =
        best == int(marks.size()) ? ui::GlyphSet::Glyphs : ui::GlyphSet::Ascii;
    check(ui::pixelGlyphSet() == expected,
          std::string("pixelGlyphSet() is ") +
              (expected == ui::GlyphSet::Ascii ? "Ascii" : "Glyphs") +
              ", which is what the best font carries (" + std::to_string(best) + " of " +
              std::to_string(marks.size()) + " marks)");
    if (ui::pixelGlyphSet() == ui::GlyphSet::Ascii) {
        check(std::strcmp(ui::icon(ui::Icon::Ok, ui::pixelGlyphSet()), "+") == 0,
              "the pixel profile answers the ASCII column while its fonts lack the marks");
    }
}

// ---------------------------------------------------------------------------
// 4. The legend, rendered through the real text backend
// ---------------------------------------------------------------------------

ImTui::TScreen g_screen;

struct Grid {
    int nx = 0;
    int ny = 0;
    std::vector<uint32_t> ch;
    std::vector<uint8_t> chwidth;
    std::vector<uint32_t> ch2;

    uint32_t at(int x, int y) const { return ch[static_cast<std::size_t>(y) * nx + x]; }
    uint8_t widthAt(int x, int y) const { return chwidth[static_cast<std::size_t>(y) * nx + x]; }
    uint32_t continuationAt(int x, int y) const {
        return ch2[static_cast<std::size_t>(y) * nx + x];
    }
    std::string rowText(int y) const {
        std::string text;
        for (int x = 0; x < nx; ++x) {
            const uint32_t cp = at(x, y);
            text += cp == 0 ? std::string(" ") : utf8Of(cp);
        }
        return text;
    }
};

Grid capture() {
    Grid g;
    g.nx = g_screen.nx;
    g.ny = g_screen.ny;
    const std::size_t cells = static_cast<std::size_t>(g.nx) * g.ny;
    g.ch.resize(cells);
    g.chwidth.resize(cells);
    g.ch2.resize(cells);
    for (std::size_t i = 0; i < cells; ++i) {
        g.ch[i] = g_screen.data[i].ch;
        g.chwidth[i] = g_screen.data[i].chwidth;
        g.ch2[i] = g_screen.data[i].ch2;
    }
    return g;
}

void dumpGrid(const Grid& g) {
    std::fprintf(stderr, "grid (%dx%d):\n", g.nx, g.ny);
    for (int y = 0; y < g.ny; ++y) {
        std::string ascii;
        for (int x = 0; x < g.nx; ++x) {
            const uint32_t cp = g.at(x, y);
            ascii.push_back((cp >= 32 && cp < 127) ? char(cp) : '.');
        }
        std::fprintf(stderr, "%2d |%s|\n", y, ascii.c_str());
    }
}

/// One legend cell: the mark, one space, the word (NFR-16: glyph and word).
std::string legendCell(int index, ui::GlyphSet set) {
    return std::string(ui::icon(static_cast<ui::Icon>(index), set)) + " " + nameOf(index);
}

/// The legend laid out for a display: one column when the width allows it, two
/// when it does not, so the 40-column screen still fits the whole table. Each cell
/// is padded to its column; the padding is part of the expected row text, so a
/// cell that overflowed its column would show up as a shifted label.
struct Legend {
    int columns = 1;
    int columnWidth = 0;
    int rowsPerColumn = 0;
    std::vector<std::string> cells;
    std::vector<std::string> rows; ///< each one padded to the full display width
};

Legend buildLegend(int nx, ui::GlyphSet set) {
    Legend legend;
    legend.cells.reserve(ui::kIconCount);
    for (int i = 0; i < ui::kIconCount; ++i)
        legend.cells.push_back(legendCell(i, set));

    legend.columns = nx >= 60 ? 1 : 2;
    legend.columnWidth = nx / legend.columns;
    legend.rowsPerColumn = (ui::kIconCount + legend.columns - 1) / legend.columns;

    for (int row = 0; row < legend.rowsPerColumn; ++row) {
        std::string text;
        for (int column = 0; column < legend.columns; ++column) {
            const int index = column * legend.rowsPerColumn + row;
            std::string cell = index < ui::kIconCount
                                   ? legend.cells[static_cast<std::size_t>(index)]
                                   : std::string();
            const int cells = ui::displayWidth(cell);
            check(cells <= legend.columnWidth, "the legend cell " + describe(cell) + " fits its " +
                                                   std::to_string(legend.columnWidth) +
                                                   "-cell column on a " + std::to_string(nx) +
                                                   "-column display");
            // Pad the cell to its column: spaces only, so the label column of the
            // second block stays where the first block's ends.
            text += cell;
            text.append(static_cast<std::size_t>(
                            cells < legend.columnWidth ? legend.columnWidth - cells : 0),
                        ' ');
        }
        legend.rows.push_back(text);
    }
    return legend;
}

/// Render the legend once and read the cell grid back.
Grid renderLegend(const Legend& legend, int nx, int ny) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(nx), float(ny));
    io.DeltaTime = 1.0f / 60.0f;

    ImTui_ImplText_NewFrame();
    ImGui::NewFrame();
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const ImU32 ink = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    for (std::size_t row = 0; row < legend.rows.size(); ++row) {
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(0.0f, static_cast<float>(row)),
                      ink, legend.rows[row].c_str());
    }
    ImGui::Render();
    ImTui_ImplText_RenderDrawData(ImGui::GetDrawData(), &g_screen);
    return capture();
}

std::string padTo(std::string text, int cells) {
    const int used = ui::displayWidth(text);
    if (used < cells)
        text.append(static_cast<std::size_t>(cells - used), ' ');
    return text;
}

/// Where the first legend row landed. The renderer decides the pen-to-row mapping;
/// the harness only requires that the rows are consecutive and that both modes
/// agree, which is the property the shell depends on.
int findFirstRow(const Grid& grid, const std::string& expected) {
    const std::string wanted = padTo(expected, grid.nx);
    for (int y = 0; y < grid.ny; ++y) {
        if (grid.rowText(y) == wanted)
            return y;
    }
    return -1;
}

struct ScreenCase {
    int firstRow = -1;
    std::vector<int> nameColumns; ///< label start column per icon, in table order
};

/// The mode as a word, for labels: the set is what the legend is built from now, so
/// the harness never touches `LLMFUN_ASCII` (and cannot leak it into a later check).
const char* setLabel(ui::GlyphSet set) { return set == ui::GlyphSet::Ascii ? "ASCII" : "Unicode"; }

ScreenCase checkScreen(int nx, int ny, ui::GlyphSet set, const char* dumpDir) {
    ScreenCase result;
    const Legend legend = buildLegend(nx, set);
    const std::string label =
        std::string(setLabel(set)) + " at " + std::to_string(nx) + "x" + std::to_string(ny);

    const Grid grid = renderLegend(legend, nx, ny);
    check(grid.nx == nx && grid.ny == ny,
          label + ": the display is " + std::to_string(grid.nx) + "x" + std::to_string(grid.ny));
    if (grid.nx != nx || grid.ny != ny) {
        dumpGrid(grid);
        return result;
    }

    // The rows are exactly the legend, cell for cell: a "?" substitution, a
    // dropped glyph or a stray cell all fail here.
    result.firstRow = findFirstRow(grid, legend.rows.front());
    check(result.firstRow >= 0,
          label + ": the first legend row " + describe(legend.rows.front()) + " is on the screen");
    if (result.firstRow < 0) {
        dumpGrid(grid);
        return result;
    }
    for (std::size_t row = 0; row < legend.rows.size(); ++row) {
        const int y = result.firstRow + static_cast<int>(row);
        const std::string got = y < grid.ny ? grid.rowText(y) : std::string();
        check(y < grid.ny && got == padTo(legend.rows[row], nx),
              label + ": row " + std::to_string(row) + " is " +
                  describe(padTo(legend.rows[row], nx)) + " (got " + describe(got) + ")");
    }

    // Each icon occupies exactly one cell as the backend measures it, and the
    // label starts one space later - in both modes, which is the alignment the
    // fallback has to preserve.
    for (int i = 0; i < ui::kIconCount; ++i) {
        const int column = i / legend.rowsPerColumn;
        const int x = column * legend.columnWidth;
        const int y = result.firstRow + (i % legend.rowsPerColumn);
        const std::string mark = ui::icon(static_cast<ui::Icon>(i), set);
        const std::vector<uint32_t> markParts = codepoints(mark);
        const std::string name = std::string("icon ") + nameOf(i);

        check(y < grid.ny && grid.at(x, y) == markParts.front(),
              label + ": " + name + " draws " + hexCodepoint(markParts.front()) + " in column " +
                  std::to_string(x));
        check(y < grid.ny && grid.widthAt(x, y) == 1,
              label + ": " + name + " is one cell wide as the renderer measures it (" +
                  std::to_string(y < grid.ny ? grid.widthAt(x, y) : 0) + ")");
        check(y < grid.ny && grid.continuationAt(x, y) == 0,
              label + ": " + name + " has no continuation codepoint");

        // The label column: exactly one blank cell after the mark, then the word.
        // The word is read back out of the *grid* rather than compared with the
        // row string the same builder produced, so a layout that drew the word one
        // column off cannot pass by having shifted both sides of the comparison.
        const int markEnd = x + static_cast<int>(markParts.size());
        const int nameColumn = markEnd + 1;
        result.nameColumns.push_back(nameColumn);
        const uint32_t follower = y < grid.ny ? grid.at(markEnd, y) : 0;
        check(follower == 0 || follower == ' ', label + ": " + name +
                                                    " is followed by one blank cell (" +
                                                    hexCodepoint(follower) + ")");

        std::string drawnName;
        const std::string expectedName = nameOf(i);
        if (y < grid.ny) {
            for (std::size_t c = 0; c < expectedName.size(); ++c) {
                const int cell = nameColumn + static_cast<int>(c);
                const uint32_t cp = cell < grid.nx ? grid.at(cell, y) : 0;
                // A blank cell - nothing drawn there - reads as a space, the way the
                // row text reads it, so a failure message shows what the eye sees.
                drawnName += cp == 0 ? std::string(" ") : utf8Of(cp);
            }
        }
        check(drawnName == expectedName, label + ": " + name + " reads " + describe(expectedName) +
                                             " from column " + std::to_string(nameColumn) +
                                             " (the screen draws " + describe(drawnName) + ")");
    }

    // No missing-glyph substitution anywhere on an ASCII screen: "?" is what a
    // console paints for a codepoint its font does not have, and U+FFFD is what a
    // decoder paints for bytes it could not read.
    if (set == ui::GlyphSet::Ascii) {
        int questionMarks = 0;
        int replacements = 0;
        for (int y = 0; y < grid.ny; ++y) {
            for (int x = 0; x < grid.nx; ++x) {
                questionMarks += grid.at(x, y) == '?' ? 1 : 0;
                replacements += grid.at(x, y) == 0xFFFD ? 1 : 0;
            }
        }
        check(questionMarks == 0,
              label + ": no \"?\" substitution (" + std::to_string(questionMarks) + ")");
        check(replacements == 0,
              label + ": no U+FFFD replacement (" + std::to_string(replacements) + ")");
    }

    if (dumpDir != nullptr) {
        const std::string path = std::string(dumpDir) + "/icons-" +
                                 (set == ui::GlyphSet::Ascii ? "ascii" : "unicode") + "-" +
                                 std::to_string(nx) + "x" + std::to_string(ny) + ".txt";
        std::FILE* file = std::fopen(path.c_str(), "w");
        if (file == nullptr) {
            check(false, label + ": cannot write " + path);
        } else {
            for (int y = 0; y < grid.ny; ++y)
                std::fprintf(file, "%s\n", grid.rowText(y).c_str());
            std::fclose(file);
            std::fprintf(stderr, "%s: wrote %s\n", label.c_str(), path.c_str());
        }
    }

    return result;
}

void checkScreens(const char* dumpDir) {
    // The two text-golden sizes task 19 will keep (design §7.1: 80x24 and the 40x15
    // floor). Both sets at both sizes, then the two compared.
    ScreenCase unicode80;
    ScreenCase ascii80;
    ScreenCase unicode40;
    ScreenCase ascii40;
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImTui_ImplText_Init();

        unicode80 = checkScreen(80, 24, ui::GlyphSet::Glyphs, dumpDir);
        ascii80 = checkScreen(80, 24, ui::GlyphSet::Ascii, dumpDir);
        unicode40 = checkScreen(40, 15, ui::GlyphSet::Glyphs, dumpDir);
        ascii40 = checkScreen(40, 15, ui::GlyphSet::Ascii, dumpDir);

        ImTui_ImplText_Shutdown();
        ImGui::DestroyContext();
    }

    // Alignment parity is the fallback's whole job: the same rows, the same
    // label columns, whichever mode the terminal gets.
    const struct {
        const char* what;
        const ScreenCase& unicode;
        const ScreenCase& ascii;
    } pairs[] = {
        {"80x24", unicode80, ascii80},
        {"40x15", unicode40, ascii40},
    };
    for (const auto& pair : pairs) {
        const std::string label = std::string("the legend at ") + pair.what;
        check(pair.unicode.firstRow >= 0 && pair.unicode.firstRow == pair.ascii.firstRow,
              label + ": both modes start the legend on the same row (" +
                  std::to_string(pair.unicode.firstRow) + " vs " +
                  std::to_string(pair.ascii.firstRow) + ")");
        check(pair.unicode.nameColumns.size() == pair.ascii.nameColumns.size(),
              label + ": both modes draw every icon");
        for (std::size_t i = 0;
             i < pair.unicode.nameColumns.size() && i < pair.ascii.nameColumns.size(); ++i) {
            const ui::Icon which = static_cast<ui::Icon>(i);
            // The invariant is "the label starts right after the mark", not "in the
            // same column": keep the mark one cell wide and the columns match, and
            // the one mark whose fallback is wider (Ellipsis, three dots) pushes
            // its own label along by exactly that much instead of shifting anyone
            // else's.
            const int expectedShift =
                ui::displayWidth(ui::asciiFallback(which)) - ui::displayWidth(ui::glyph(which));
            check(pair.unicode.nameColumns[i] + expectedShift == pair.ascii.nameColumns[i],
                  label + ": " + nameOf(static_cast<int>(i)) + " puts its label in column " +
                      std::to_string(pair.unicode.nameColumns[i]) + " with the glyph and " +
                      std::to_string(pair.ascii.nameColumns[i]) + " in ASCII (shift " +
                      std::to_string(expectedShift) + ")");
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    // The one-cell rule is the emoji rule only in the renderer's default mode, and
    // this binary owns its process: a terminal configured to draw emoji narrow must
    // not turn "no emoji in the glyph table" into a check that passes either way.
    // Never set it in a shell profile and run the tests through it.
    ::unsetenv("LLMFUN_IMTUI_EMOJI_WIDTH");

    const char* dumpDir = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0 || std::strcmp(argv[i], "--verbose") == 0) {
            g_verbose = true;
        } else if (std::strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
            dumpDir = argv[++i];
        } else {
            std::fprintf(stderr, "usage: %s [-v] [--dump DIR]\n", argv[0]);
            return 2;
        }
    }
    if (dumpDir != nullptr) {
        struct stat info;
        if (::stat(dumpDir, &info) != 0 || !S_ISDIR(info.st_mode)) {
            std::fprintf(stderr, "usage: %s [-v] [--dump DIR] (DIR must exist)\n", argv[0]);
            return 2;
        }
    }

    checkTable();
    checkCutMarker();
    checkDetection();
    checkAccessor();
    checkGlyphSets();
    checkScreens(dumpDir);

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("test_icons: all checks passed\n");
    return 0;
}
