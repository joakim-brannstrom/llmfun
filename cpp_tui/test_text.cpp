// test_text.cpp
//
// Unit test for the text and width module (task 14, design §4.15 "Text"): the
// one place that measures, cuts and pads text in terminal cells.
//
// What is pinned here, case by case:
//
//   0. decoding       measuring is total: empty input, ASCII, and the byte
//                     sequences that are not text at all (a lone continuation
//                     byte, an overlong form, a truncated sequence, a surrogate
//                     written out, a 5-byte lead). Every invalid byte measures
//                     one cell and the walk still ends;
//
//   1. the table      wide = 2, combining = 0, and a boundary pair for every
//                     range in the table (the codepoint just outside each bound
//                     included), because a mistyped bound is invisible in a
//                     rendered screen but shifts a column;
//
//   2. the sweep      every codepoint in U+0000..U+10FFFF against an
//                     independent oracle built from the same reference data as
//                     the table the ncurses backend renders with
//                     (`imtui_wcwidth`, vendor/imgui/imgui/imgui_draw.cpp);
//                     `imtui_utf8_grid_test` drives the real table through the
//                     real backend, so the two directions meet;
//
//   3. truncate       fits -> the same bytes; the cut lands between codepoints;
//                     the ellipsis is accounted for (and cut itself when it is
//                     the only thing that does not fit); a wide glyph is never
//                     halved; a combining mark stays with its base; a zero or
//                     negative budget answers nothing;
//
//   3b. the marker   `truncate(text, cells)` marks the cut with
//                     `ui::icon(Icon::Ellipsis)`, so the cut follows the glyph
//                     table's own fallback ("…" or "...") - checked in both modes,
//                     because the two modules own one decision between them;
//
//   4. wrap           one entry per line (`count('\n') + 1`), greedy at spaces,
//                     hard-broken inside a word that cannot fit, never wider
//                     than the budget unless a single glyph is wider, every entry
//                     a view into the input;
//
//   5. padRight       pads to the budget in cells, never cuts, never shrinks;
//
//   6. formatCount    the compact numerals the chips and the rail's time column
//                     draw: one decimal below ten units, none above, no "10.0k",
//                     rounding carries into the next unit, sign kept.
//
// Layering check, by construction: this binary compiles ui/text.cpp and ui/icons.cpp
// directly and links nothing else, so neither module can have grown an ImGui, tui.h
// or v4 dependency (icons.cpp is here because truncate()'s default marker is
// ui::icon(Icon::Ellipsis), and both are standard-library-only modules).
//
// Exit codes: 0 = every check passed; 1 = at least one failed; 2 = usage error.
// Headless. Run from the build dir: ./test_text [-v]

#include "ui/icons.h"
#include "ui/text.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace ui = llmfun::tui::ui;

namespace {

int g_failures = 0;
bool g_verbose = false;

/// Text as it will read in a failure message: quotes around it, control bytes
/// and the quote/backslash escaped, everything else (including a UTF-8 glyph)
/// left alone so a CJK fixture is recognisable.
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

void check(bool condition, const std::string& what) {
    if (condition) {
        if (g_verbose) {
            std::fprintf(stderr, "ok   %s\n", what.c_str());
        }
        return;
    }
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++g_failures;
}

void expectCells(int got, int want, const std::string& what) {
    check(got == want,
          what + ": got " + std::to_string(got) + " cells, expected " + std::to_string(want));
}

void expectText(std::string_view got, std::string_view want, const std::string& what) {
    check(got == want, what + ": got " + describe(got) + ", expected " + describe(want));
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

/// Four cells, two per glyph.
constexpr const char* kCjk = "中文";
/// 'e' + U+0301 COMBINING ACUTE ACCENT: one cell, two codepoints.
constexpr const char* kAccented = "e\xCC\x81";
/// U+2705 WHITE HEAVY CHECK MARK, in the emoji-presentation class.
constexpr const char* kEmoji = "✅";
/// U+2026 HORIZONTAL ELLIPSIS, the default cut marker, one cell.
constexpr const char* kEllipsis = "…";
/// A sentence with words, for the wrapping cases.
constexpr const char* kSentence = "the quick brown fox";

// ---------------------------------------------------------------------------
// Independent width oracle
//
// Same reference data as the table in ui/text.cpp - Markus Kuhn's wcwidth()
// classes, the zero-width format blocks, East Asian Wide/Fullwidth, and
// emoji-data.txt 15.1's Emoji_Presentation=Yes class - but written from that
// data as one flat sorted range table with a linear scan, not as the module's
// class-by-class ladder. The §2 sweep compares the two over every codepoint, so
// a mistyped bound on either side fails the run.
// ---------------------------------------------------------------------------

struct Range {
    uint32_t first;
    uint32_t last;
};

constexpr Range kZeroWidth[] = {
    {0x0000, 0x0000}, // NUL
    {0x0001, 0x001F}, // C0 controls
    {0x007F, 0x009F}, // DEL..C1 (NBSP U+00A0 is a normal space)
    {0x0300, 0x036F}, // combining diacritical marks
    {0x1AB0, 0x1AFF}, // combining diacritical marks extended
    {0x1DC0, 0x1DFF}, // combining diacritical marks supplement
    {0x200B, 0x200F}, // ZWSP, ZWNJ, ZWJ, LRM, RLM
    {0x2028, 0x202E}, // line/paragraph separators, bidi controls
    {0x2060, 0x206F}, // word joiner, invisible operators
    {0x20D0, 0x20FF}, // combining marks for symbols
    {0xFE00, 0xFE0F}, // variation selectors (incl. VS16)
    {0xFE20, 0xFE2F}, // combining half marks
    {0xFEFF, 0xFEFF}, // BOM / ZWNBSP
    {0xFFF9, 0xFFFB}, // interlinear annotation anchors
};

/// East Asian Wide/Fullwidth: two cells on every terminal that follows UAX #11.
constexpr Range kWide[] = {
    {0x1100, 0x115F},   // Hangul Jamo lead consonants
    {0x2329, 0x232A},   // angle brackets
    {0x2E80, 0xA4CF},   // CJK radicals .. Yi
    {0xA960, 0xA97C},   // Hangul Jamo Extended-A
    {0xAC00, 0xD7A3},   // Hangul syllables
    {0xF900, 0xFAFF},   // CJK compatibility ideographs
    {0xFE10, 0xFE19},   // vertical forms
    {0xFE30, 0xFE6F},   // CJK compatibility forms
    {0xFF01, 0xFF60},   // fullwidth forms
    {0xFFE0, 0xFFE6},   // fullwidth signs
    {0x20000, 0x2FFFD}, // CJK extension B..F
    {0x30000, 0x3FFFD}, // CJK extension G
};

/// Emoji_Presentation=Yes (emoji-data.txt 15.1, 2023-09): two cells without a
/// variation selector, and one on a narrow-emoji terminal
/// (LLMFUN_IMTUI_EMOJI_WIDTH=1, the backend's own workaround).
constexpr Range kEmojiClass[] = {
    {0x231A, 0x231B},   {0x23E9, 0x23F3},   {0x23F8, 0x23FA},   {0x25FD, 0x25FE},
    {0x2614, 0x2615},   {0x2648, 0x2653},   {0x267F, 0x267F},   {0x2693, 0x2693},
    {0x26A1, 0x26A1},   {0x26AA, 0x26AB},   {0x26BD, 0x26BE},   {0x26C4, 0x26C5},
    {0x26CE, 0x26CE},   {0x26D4, 0x26D4},   {0x26EA, 0x26EA},   {0x26F2, 0x26F3},
    {0x26F5, 0x26F5},   {0x26FA, 0x26FA},   {0x26FD, 0x26FD},   {0x2705, 0x2705},
    {0x270A, 0x270B},   {0x2728, 0x2728},   {0x274C, 0x274C},   {0x274E, 0x274E},
    {0x2753, 0x2755},   {0x2757, 0x2757},   {0x2795, 0x2797},   {0x27B0, 0x27B0},
    {0x27BF, 0x27BF},   {0x2B1B, 0x2B1C},   {0x2B50, 0x2B50},   {0x2B55, 0x2B55},
    {0x1F000, 0x1F644}, {0x1F300, 0x1F6FF}, {0x1F7E0, 0x1F7EB}, {0x1F7F0, 0x1F7F0},
    {0x1F900, 0x1F9FF}, {0x1FA70, 0x1FAFF},
};

bool inRanges(const Range* ranges, int count, uint32_t cp) {
    for (int i = 0; i < count; ++i) {
        if (cp >= ranges[i].first && cp <= ranges[i].last) {
            return true;
        }
    }
    return false;
}

bool emojiWidthOne() {
    const char* value = std::getenv("LLMFUN_IMTUI_EMOJI_WIDTH");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

int oracleWidth(uint32_t cp, bool narrowEmoji) {
    if (inRanges(kZeroWidth, int(sizeof(kZeroWidth) / sizeof(kZeroWidth[0])), cp)) {
        return 0;
    }
    // A flag is a grapheme cluster the grid cannot represent: one cell per
    // regional indicator, and that has to win over the coarse emoji block.
    if (cp >= 0x1F1E6 && cp <= 0x1F1FF) {
        return 1;
    }
    if (inRanges(kWide, int(sizeof(kWide) / sizeof(kWide[0])), cp)) {
        return 2;
    }
    if (inRanges(kEmojiClass, int(sizeof(kEmojiClass) / sizeof(kEmojiClass[0])), cp)) {
        return narrowEmoji ? 1 : 2;
    }
    return 1;
}

/// The UTF-8 encoding of `cp` (the test only feeds it codepoints that are valid
/// to encode - the surrogates are skipped by the caller).
std::string utf8Of(uint32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return out;
}

/// Visible glyphs in a byte string: codepoints whose width is not zero, walked
/// by skipping continuation bytes (the fixtures here are well-formed UTF-8).
///
/// The wrap invariant exempts a line that is a *single* glyph wider than the
/// budget, so the count has to be of glyphs a reader would see - a byte count
/// would call "e" + a combining acute two glyphs (it is one), and the exemption
/// would then wrongly admit lines that hold a base and its mark.
int visibleGlyphCount(std::string_view text) {
    int glyphs = 0;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t next = at + 1;
        while (next < text.size() && (static_cast<unsigned char>(text[next]) & 0xC0) == 0x80) {
            ++next;
        }
        if (ui::displayWidth(text.substr(at, next - at)) > 0) {
            ++glyphs;
        }
        at = next;
    }
    return glyphs;
}

// ---------------------------------------------------------------------------
// 0. Decoding: measuring is total
// ---------------------------------------------------------------------------

void checkDecoding() {
    expectCells(ui::displayWidth(""), 0, "empty input has no cells");
    expectCells(ui::displayWidth("abc"), 3, "ASCII is one cell per byte");
    expectCells(ui::displayWidth("a b"), 3, "a space is one cell");
    expectCells(ui::displayWidth(kCjk), 4, "two CJK glyphs are four cells");
    expectCells(ui::displayWidth(kAccented), 1, "'e' plus a combining acute is one cell");
    expectCells(ui::displayWidth(std::string(kCjk) + "a"), 5, "wide and narrow mix by the cell");

    // Invalid input: one cell for the sequence, consuming what its lead claims -
    // the renderer's rule (measured against the real ImTui backend: each of
    // these strings draws exactly the number of cells asserted here).
    expectCells(ui::displayWidth("\x80"), 1, "a lone continuation byte is one cell");
    expectCells(ui::displayWidth("a\x80"
                                 "b"),
                3, "a bad byte between letters is one cell");
    expectCells(ui::displayWidth("\xC0\x80"), 1, "an overlong two-byte form is one cell");
    expectCells(ui::displayWidth("\xE4\xB8"), 1, "a truncated three-byte sequence is one cell");
    expectCells(ui::displayWidth("\xFF"), 1, "an invalid lead byte is one cell");
    expectCells(ui::displayWidth("\xF5\x80\x80\x80"), 1,
                "an out-of-range four-byte form is one cell");
    expectCells(ui::displayWidth("\xED\xA0\x80"), 1, "a surrogate written out is one cell");
    expectCells(ui::displayWidth("a\xE4\xB8"
                                 "b"),
                2, "a truncated glyph swallows the byte the renderer swallows");
    expectCells(ui::displayWidth(std::string(1000, '\x80')), 1000,
                "1000 stray bytes measure 1000 cells (the walk cannot stall)");

    // A well-formed sequence is still one glyph of its own width.
    expectCells(ui::displayWidth("\xC3\xA9"), 1, "a valid two-byte e-acute is one cell");
    expectCells(ui::displayWidth("\xE4\xB8\xAD"), 2, "a valid U+4E2D is two cells");
    expectCells(ui::displayWidth("\xF0\xA0\x80\x80"), 2, "a valid U+20000 is two cells");

    // Control characters take no cell of their own.
    expectCells(ui::displayWidth("a\nb"), 2, "a newline measures no cells");
    expectCells(ui::displayWidth("a\r\nb"), 2, "a carriage return measures no cells either");
    expectCells(ui::displayWidth(std::string("a\0b", 3)), 2,
                "an embedded NUL measures nothing and does not end the text");
    // A zero byte also ends what an invalid sequence claims, like the renderer's
    // "count the non-zero bytes" clamp: E4 then NUL then 'A' is 1 + 0 + 1 cells.
    expectCells(ui::displayWidth(std::string("\xE4\0"
                                             "A",
                                             3)),
                2, "a zero byte stops the invalid sequence instead of being swallowed");
}

// ---------------------------------------------------------------------------
// 1. The width table
// ---------------------------------------------------------------------------

struct CellCase {
    uint32_t cp;
    int cells;
    bool emojiClass; // expected becomes 1 with LLMFUN_IMTUI_EMOJI_WIDTH=1
    const char* note;
};

constexpr CellCase kCellCases[] = {
    {0x0000, 0, false, "NUL"},
    {0x001F, 0, false, "last C0 control"},
    {0x0020, 1, false, "space"},
    {0x007E, 1, false, "tilde"},
    {0x007F, 0, false, "DEL"},
    {0x009F, 0, false, "last C1 control"},
    {0x00A0, 1, false, "NBSP is a normal one-cell space"},
    {0x00E9, 1, false, "precomposed e-acute"},
    {0x02FF, 1, false, "just below the combining block"},
    {0x0300, 0, false, "first combining diacritical mark"},
    {0x036F, 0, false, "last combining diacritical mark"},
    {0x0370, 1, false, "just above the combining block"},
    {0x1AAF, 1, false, "just below combining extended"},
    {0x1AB0, 0, false, "combining extended first"},
    {0x1AFF, 0, false, "combining extended last"},
    {0x1DC0, 0, false, "combining supplement first"},
    {0x1DFF, 0, false, "combining supplement last"},
    {0x10FF, 1, false, "just below Hangul Jamo"},
    {0x1100, 2, false, "Hangul Jamo first"},
    {0x115F, 2, false, "Hangul Jamo last"},
    {0x1160, 1, false, "Hangul Jamo filler is not wide"},
    {0x200A, 1, false, "just below the zero-width format block"},
    {0x200B, 0, false, "zero-width space"},
    {0x200D, 0, false, "zero-width joiner"},
    {0x200F, 0, false, "right-to-left mark"},
    {0x2010, 1, false, "hyphen is one cell"},
    {0x2028, 0, false, "line separator"},
    {0x202E, 0, false, "right-to-left override"},
    {0x202F, 1, false, "narrow no-break space is one cell"},
    {0x2060, 0, false, "word joiner"},
    {0x206F, 0, false, "last invisible operator"},
    {0x2070, 1, false, "superscript zero"},
    {0x20CF, 1, false, "just below combining marks for symbols"},
    {0x20D0, 0, false, "combining mark for symbols first"},
    {0x20FF, 0, false, "combining mark for symbols last"},
    {0x2100, 1, false, "account-of sign"},
    {0x2192, 1, false, "rightwards arrow is narrow"},
    {0x231A, 2, true, "watch (emoji-presentation)"},
    {0x2328, 1, false, "keyboard (text default)"},
    {0x2329, 2, false, "left angle bracket (wide)"},
    {0x232A, 2, false, "right angle bracket (wide)"},
    {0x232B, 1, false, "just above the angle brackets"},
    {0x2500, 1, false, "box-drawing horizontal is one cell"},
    {0x2588, 1, false, "full block is one cell"},
    {0x26A0, 1, false, "warning sign is narrow without VS16"},
    {0x2705, 2, true, "white heavy check mark (emoji-presentation)"},
    {0x2800, 1, false, "braille blank is one cell (spinner glyphs)"},
    {0x2B05, 1, false, "leftwards arrow is text-default, narrow"},
    {0x2E7F, 1, false, "just below the CJK radical block"},
    {0x2E80, 2, false, "CJK radical first"},
    {0x4E2D, 2, false, "CJK ideograph"},
    {0xA4CF, 2, false, "last of the CJK/Yi block"},
    {0xA4D0, 1, false, "just above the CJK/Yi block"},
    {0xA95F, 1, false, "just below Hangul Jamo Extended-A"},
    {0xA960, 2, false, "Hangul Jamo Extended-A first"},
    {0xA97C, 2, false, "Hangul Jamo Extended-A last"},
    {0xA97D, 1, false, "just above Hangul Jamo Extended-A"},
    {0xABFF, 1, false, "just below Hangul syllables"},
    {0xAC00, 2, false, "Hangul syllables first"},
    {0xD7A3, 2, false, "Hangul syllables last"},
    {0xD7A4, 1, false, "just above Hangul syllables"},
    {0xF8FF, 1, false, "just below CJK compatibility ideographs"},
    {0xF900, 2, false, "CJK compatibility ideographs first"},
    {0xFAFF, 2, false, "CJK compatibility ideographs last"},
    {0xFB00, 1, false, "latin ligature"},
    {0xFE0F, 0, false, "VS16 variation selector"},
    {0xFE10, 2, false, "vertical forms first"},
    {0xFE19, 2, false, "vertical forms last"},
    {0xFE1A, 1, false, "just above the vertical forms"},
    {0xFE1F, 1, false, "just below the combining half marks"},
    {0xFE20, 0, false, "combining half marks first"},
    {0xFE2F, 0, false, "combining half marks last"},
    {0xFE30, 2, false, "CJK compatibility forms first"},
    {0xFE6F, 2, false, "CJK compatibility forms last"},
    {0xFE70, 1, false, "just above the CJK compatibility forms"},
    {0xFEFF, 0, false, "BOM / zero-width no-break space"},
    {0xFF00, 1, false, "just below the fullwidth forms"},
    {0xFF01, 2, false, "fullwidth exclamation mark"},
    {0xFF60, 2, false, "last of the fullwidth forms"},
    {0xFF61, 1, false, "halfwidth ideographic full stop"},
    {0xFFDF, 1, false, "just below the fullwidth signs"},
    {0xFFE0, 2, false, "fullwidth cent sign"},
    {0xFFE6, 2, false, "last fullwidth sign"},
    {0xFFE7, 1, false, "just above the fullwidth signs"},
    {0xFFF9, 0, false, "interlinear annotation anchor first"},
    {0xFFFB, 0, false, "interlinear annotation anchor last"},
    {0xFFFC, 1, false, "object replacement character"},
    {0x1F1E5, 2, true, "just below the regional indicators (coarse emoji block)"},
    {0x1F1E6, 1, false, "regional indicator A is one cell"},
    {0x1F1FF, 1, false, "regional indicator Z is one cell"},
    {0x1F200, 2, true, "squared katakana (coarse emoji block)"},
    {0x1F300, 2, true, "cyclone (emoji-presentation)"},
    {0x1F600, 2, true, "grinning face (emoji-presentation)"},
    {0x1F644, 2, true, "last of the coarse 1F000-1F644 block"},
    {0x1F645, 2, true, "face with no good gesture (1F300-1F6FF)"},
    {0x1F6FF, 2, true, "last of the transport block"},
    {0x1F700, 1, false, "alchemical symbol (not emoji-presentation)"},
    {0x1F7DF, 1, false, "just below the large colored circles"},
    {0x1F7E0, 2, true, "large orange circle"},
    {0x1F7EB, 2, true, "last large colored circle"},
    {0x1F7EC, 1, false, "just above the large colored circles"},
    {0x1F7EF, 1, false, "just below the heavy equals sign"},
    {0x1F7F0, 2, true, "heavy equals sign"},
    {0x1F7F1, 1, false, "just above the heavy equals sign"},
    {0x1F8FF, 1, false, "just below the supplemental symbols"},
    {0x1F900, 2, true, "supplemental symbols first"},
    {0x1F9FF, 2, true, "supplemental symbols last"},
    {0x1FA00, 1, false, "chess symbol (not emoji-presentation)"},
    {0x1FA6F, 1, false, "just below extended-A"},
    {0x1FA70, 2, true, "symbols and pictographs extended-A first"},
    {0x1FAFF, 2, true, "symbols and pictographs extended-A last"},
    {0x1FB00, 1, false, "just above extended-A"},
    {0x1FFFF, 1, false, "unassigned plane-1 codepoint"},
    {0x20000, 2, false, "CJK extension B first"},
    {0x2FFFD, 2, false, "CJK extension last"},
    {0x2FFFE, 1, false, "just above the CJK extensions"},
    {0x30000, 2, false, "CJK extension G first"},
    {0x3FFFD, 2, false, "CJK extension G last"},
    {0x3FFFE, 1, false, "just above CJK extension G"},
    {0x10FFFD, 1, false, "last private-use codepoint"},
};

void checkCellTable() {
    const bool narrowEmoji = emojiWidthOne();
    for (const CellCase& c : kCellCases) {
        const int want = (c.emojiClass && narrowEmoji) ? 1 : c.cells;
        const std::string what = std::string(c.note) + " (" + hexCodepoint(c.cp) + ")";
        expectCells(ui::displayWidth(utf8Of(c.cp)), want, what);
    }
    // The fixtures used by the other sections are part of the table contract.
    expectCells(ui::displayWidth(kEmoji), narrowEmoji ? 1 : 2,
                narrowEmoji ? "check-mark emoji on a narrow-emoji terminal"
                            : "check-mark emoji in the default table");
}

// ---------------------------------------------------------------------------
// 2. The sweep: every codepoint against the oracle
// ---------------------------------------------------------------------------

void checkTableSweep() {
    const bool narrowEmoji = emojiWidthOne();
    int mismatches = 0;
    std::string firstMismatch;

    for (uint32_t cp = 0; cp <= 0x10FFFF; ++cp) {
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            continue; // surrogates have no UTF-8 encoding
        }
        const std::string encoded = utf8Of(cp);
        const int got = ui::displayWidth(encoded);
        const int want = oracleWidth(cp, narrowEmoji);
        if (got == want) {
            continue;
        }
        ++mismatches;
        if (mismatches == 1) {
            firstMismatch = hexCodepoint(cp) + ": displayWidth " + std::to_string(got) +
                            ", oracle " + std::to_string(want);
        }
    }

    check(mismatches == 0, "sweep: displayWidth matches the oracle over U+0000..U+10FFFF" +
                               (mismatches == 0 ? std::string()
                                                : " - " + std::to_string(mismatches) +
                                                      " mismatch(es), first " + firstMismatch));
    if (g_verbose) {
        std::fprintf(stderr, "ok   sweep: 1.1M codepoints, emoji width %d\n", narrowEmoji ? 1 : 2);
    }
}

// ---------------------------------------------------------------------------
// 3. truncate
// ---------------------------------------------------------------------------

/// Every `truncate()` result is built from whole codepoints: either a prefix of
/// the input plus the marker, or - when the marker alone does not fit - a prefix
/// of the marker. A cut inside a sequence would satisfy neither comparison.
void checkTruncateIsPrefix(std::string_view text, int cells, const char* marker) {
    const std::string got = ui::truncate(text, cells, marker);
    const std::string_view markerText =
        marker != nullptr ? std::string_view(marker) : std::string_view();
    const std::string where = "truncate(" + describe(text) + ", " + std::to_string(cells) + ", " +
                              describe(markerText) + ")";
    if (cells > 0 && ui::displayWidth(markerText) > cells) {
        check(markerText.compare(0, got.size(), got) == 0,
              where + " answers a prefix of the marker: got " + describe(got));
    } else {
        std::string_view head = got;
        if (!markerText.empty() && got.size() >= markerText.size() &&
            got.compare(got.size() - markerText.size(), markerText.size(), markerText) == 0) {
            head = std::string_view(got.data(), got.size() - markerText.size());
        }
        check(text.compare(0, head.size(), head) == 0,
              where + " is the input's prefix plus the marker: got " + describe(got));
    }
    check(cells <= 0 || ui::displayWidth(got) <= cells,
          where + " fits the budget: got " + describe(got) + " (" +
              std::to_string(ui::displayWidth(got)) + " cells)");
}

void checkTruncate() {
    // Text that fits comes back byte for byte.
    expectText(ui::truncate("abc", 3), "abc", "a label at exactly its budget is unchanged");
    expectText(ui::truncate("abc", 10), "abc", "a label inside its budget is unchanged");
    expectText(ui::truncate("", 5), "", "empty text stays empty");
    expectText(ui::truncate(kCjk, 4), kCjk, "wide text at exactly its budget is unchanged");
    expectText(ui::truncate(kAccented, 1), kAccented,
               "a combining mark is not dropped when the base fits");

    // No room at all.
    expectText(ui::truncate("abc", 0), "", "a zero budget answers nothing");
    expectText(ui::truncate("abc", -3), "", "a negative budget answers nothing");

    // The cut, with the marker accounted for.
    expectText(ui::truncate("abcdef", 4), std::string("abc") + kEllipsis,
               "the marker takes its own cell out of the budget");
    expectText(ui::truncate("abcdef", 5), std::string("abcd") + kEllipsis,
               "one more cell keeps one more letter");
    expectText(ui::truncate("abcdef", 1), kEllipsis, "a one-cell budget keeps only the marker");
    expectText(ui::truncate("abcdef", 2, "..."), "..", "a marker that cannot fit is itself cut");
    expectText(ui::truncate("abcdef", 1, "..."), ".",
               "a one-cell budget cuts the three-dot marker to one dot");
    expectText(ui::truncate("abcdef", 3, ""), "abc", "an empty marker is a hard cut");
    expectText(ui::truncate("abcdef", 3, nullptr), "abc", "a null marker is a hard cut");

    // Wide glyphs are never halved: the prefix stops one cell short instead.
    expectText(ui::truncate(kCjk, 4), kCjk, "two wide glyphs fit four cells");
    expectText(ui::truncate(kCjk, 3), std::string("中") + kEllipsis,
               "a wide glyph plus the marker uses three of the four cells");
    expectText(ui::truncate(kCjk, 2, ""), "中", "a two-cell budget holds one wide glyph");
    expectText(ui::truncate(kCjk, 2), kEllipsis,
               "a two-cell budget leaves room for the marker only");

    // A combining mark travels with its base.
    expectText(ui::truncate(std::string(kAccented) + "x", 1, ""), kAccented,
               "the cut keeps the mark attached to its base");

    // The default marker is the glyph table's cut marker, so a terminal that gets
    // the ASCII column cuts with "..." instead of a glyph it cannot draw (the
    // binary pins LLMFUN_ASCII=0 at start-up, so the expectations above are stable;
    // checkDefaultMarker() covers the other mode).
    expectText(ui::truncate("abcdef", 5), ui::truncate("abcdef", 5, ui::icon(ui::Icon::Ellipsis)),
               "the two-argument truncate marks the cut with icon(Ellipsis)");

    // Property: for every budget, the result is a prefix that fits.
    const std::string mixed = std::string("ab") + kCjk + "cd" + kAccented + kEmoji + "ef";
    for (int cells = 0; cells <= ui::displayWidth(mixed) + 2; ++cells) {
        checkTruncateIsPrefix(mixed, cells, kEllipsis);
        checkTruncateIsPrefix(mixed, cells, "");
        checkTruncateIsPrefix(mixed, cells, "...");
    }
}

// ---------------------------------------------------------------------------
// 4. wrap
// ---------------------------------------------------------------------------

std::string joinLines(const std::vector<std::string_view>& lines) {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) {
            out += " | ";
        }
        out += std::string(lines[i]);
    }
    return out;
}

void expectLines(const std::vector<std::string_view>& got, const std::vector<std::string>& want,
                 const std::string& what) {
    bool same = got.size() == want.size();
    for (std::size_t i = 0; same && i < got.size(); ++i) {
        same = got[i] == want[i];
    }
    std::string wanted;
    for (std::size_t i = 0; i < want.size(); ++i) {
        if (i != 0) {
            wanted += " | ";
        }
        wanted += want[i];
    }
    check(same, what + ": got " + describe(joinLines(got)) + ", expected " + describe(wanted));
}

void checkWrap() {
    // Line-faithful splitting.
    expectLines(ui::wrap("", 10), {""}, "empty text is one empty line");
    expectLines(ui::wrap("a\n", 10), {"a", ""}, "a trailing newline opens a second line");
    expectLines(ui::wrap("a\n\nb", 10), {"a", "", "b"}, "an empty line between two lines survives");
    expectLines(ui::wrap("\n", 10), {"", ""}, "a lone newline is two empty lines");

    // Greedy filling.
    expectLines(ui::wrap(kSentence, 40), {kSentence}, "a line inside the budget is one line");
    expectLines(ui::wrap("hello world", 11), {"hello world"}, "eleven cells hold all eleven");
    expectLines(ui::wrap("hello world", 10), {"hello", "world"}, "the space at the break vanishes");
    expectLines(ui::wrap("ab cd", 3), {"ab", "cd"}, "a word that does not fit moves down");
    expectLines(ui::wrap("  indented", 20), {"  indented"}, "the first line keeps its indent");
    expectLines(ui::wrap("  indented", 4), {"  in", "dent", "ed"},
                "an indent wider than the room lets the word start on the line");

    // Hard breaks inside a word.
    expectLines(ui::wrap("abcdefgh", 3), {"abc", "def", "gh"}, "a long word is broken in place");
    expectLines(ui::wrap(kCjk, 4), {kCjk}, "two wide glyphs fit four cells");
    expectLines(ui::wrap(kCjk, 2), {"中", "文"}, "one wide glyph per two-cell line");

    // A single glyph wider than the whole budget still gets its own line.
    expectLines(ui::wrap(kCjk, 1), {"中", "文"},
                "a wide glyph on a one-cell line is wider than the budget, alone");

    // An indent does not buy the exception: a line that already holds cells
    // never overshoots, not even by the wide glyph that would not fit.
    expectLines(ui::wrap(" 中", 3), {" 中"}, "an indent and a wide glyph fit three cells exactly");
    expectLines(ui::wrap(" 中", 2), {" ", "中"}, "one cell of indent cannot hold a two-cell glyph");
    expectLines(ui::wrap("  中", 3), {"  ", "中"}, "two cells of indent cannot either");
    expectLines(ui::wrap("  中", 4), {"  中"}, "with four cells the glyph joins the indent");
    // A tab breaks a line but costs no cell (controls are zero width in the
    // table), so it cannot eat the budget the way an indent does.
    expectLines(ui::wrap("\t中", 1), {"\t中"}, "a tab is a break opportunity that costs no cell");

    // A line of only spaces is the degenerate case that must still terminate.
    expectLines(ui::wrap("   x", 2), {"  ", " x"}, "an over-long space run is split, not dropped");

    // A carriage return is kept as data (the header says so); it costs no cell.
    expectLines(ui::wrap("a\r\nb", 8), {"a\r", "b"}, "a CR before the LF stays in the line");

    // No lines at all without room.
    check(ui::wrap("abc", 0).empty(), "a zero budget answers no lines");
    check(ui::wrap("abc", -4).empty(), "a negative budget answers no lines");

    // Invariants over a corpus of the shapes the shell wraps.
    const std::vector<std::string> corpus = {
        kSentence,
        std::string("中文段落没有空格，只能按单元切断。"),
        std::string("a\n\nb\n"),
        std::string("  indented\ntext"),
        std::string("src/very/long/path/to/a/file.cpp"),
        std::string(kEmoji) + " done " + kCjk + " ok",
        std::string("tab\tseparated\tvalues"),
        std::string("a") + kAccented + "b " + kCjk,
        std::string(" 中"),
        std::string("  中"),
        std::string("\t") + kCjk,
        std::string(" ") + kEmoji,
        std::string(kCjk) + kAccented + kCjk,
    };
    const int budgets[] = {1, 2, 3, 5, 8, 40};
    for (const std::string& text : corpus) {
        for (const int cells : budgets) {
            const std::vector<std::string_view> lines = ui::wrap(text, cells);
            const std::string where = "wrap(" + describe(text) + ", " + std::to_string(cells) + ")";
            check(!lines.empty(), where + " answers at least one line");

            int newlines = 1;
            for (const char c : text) {
                if (c == '\n') {
                    ++newlines;
                }
            }
            check(static_cast<int>(lines.size()) >= newlines,
                  where + " keeps every source line (" + std::to_string(lines.size()) + " for " +
                      std::to_string(newlines) + ")");

            std::size_t searchFrom = 0;
            for (const std::string_view line : lines) {
                const bool inside = line.data() >= text.data() &&
                                    line.data() + line.size() <= text.data() + text.size();
                check(inside, where + " returns views into the input, not copies");
                const std::size_t at = text.find(line, searchFrom);
                check(at != std::string::npos, where + " keeps the byte order: " + describe(line) +
                                                   " is not after the "
                                                   "previous line");
                if (at != std::string::npos) {
                    searchFrom = at + line.size();
                }
                const int width = ui::displayWidth(line);
                check(width <= cells || visibleGlyphCount(line) == 1,
                      where + " line " + describe(line) + " is " + std::to_string(width) +
                          " cells and holds more than one glyph");
            }
        }
    }

    // A run with no break opportunity is the quadratic trap: the token scan has
    // to stop at the first codepoint that does not fit instead of walking the
    // rest of the run once per line (64 KB measured 390 ms before that exit).
    // No timing assertion - a clock-based test flakes - but the shape is
    // exercised, so a regression shows up in the suite's own runtime.
    const std::string longRun(64 * 1024, 'x');
    const std::vector<std::string_view> longLines = ui::wrap(longRun, 80);
    check(longLines.size() == (longRun.size() + 79) / 80,
          "a 64 KB run wraps into " + std::to_string((longRun.size() + 79) / 80) + " lines, got " +
              std::to_string(longLines.size()));
    check(!longLines.empty() && longLines.front().size() == 80 && longLines.back().size() == 16,
          "the 64 KB run's first line is 80 bytes and its last is 16");
    bool longRunFits = true;
    for (const std::string_view line : longLines) {
        longRunFits = longRunFits && ui::displayWidth(line) <= 80;
    }
    check(longRunFits, "no line of the 64 KB run is wider than 80 cells");

    // The realistic shape the review timed: prose followed by one long word.
    const std::string proseThenWord = std::string(kSentence) + " " + std::string(16 * 1024, 'p');
    const std::vector<std::string_view> mixedLines = ui::wrap(proseThenWord, 80);
    check(!mixedLines.empty() && mixedLines.front() == kSentence,
          "prose before a long word keeps its own line");
    bool mixedFits = true;
    for (const std::string_view line : mixedLines) {
        mixedFits = mixedFits && ui::displayWidth(line) <= 80;
    }
    check(mixedFits, "no line of the prose-plus-long-word case is wider than 80 cells");
}

// ---------------------------------------------------------------------------
// 5. padRight
// ---------------------------------------------------------------------------

void checkPadRight() {
    expectText(ui::padRight("abc", 5), "abc  ", "three letters pad to five cells");
    expectText(ui::padRight("abc", 3), "abc", "an exact fit is unchanged");
    expectText(ui::padRight("abc", 1), "abc", "padding never cuts");
    expectText(ui::padRight("", 2), "  ", "empty text pads to two spaces");
    expectText(ui::padRight(kCjk, 5), std::string(kCjk) + " ", "wide text pads by its cells");
    expectText(ui::padRight("abc", -1), "abc", "a negative budget changes nothing");

    const std::vector<std::string> samples = {"", "a", "abc", kCjk, kAccented, "a b"};
    for (const std::string& text : samples) {
        for (int cells = 0; cells <= 8; ++cells) {
            const std::string padded = ui::padRight(text, cells);
            const int width = ui::displayWidth(padded);
            check(width == std::max(cells, ui::displayWidth(text)),
                  "padRight(" + describe(text) + ", " + std::to_string(cells) + ") is " +
                      std::to_string(width) + " cells");
            check(padded.compare(0, text.size(), text) == 0,
                  "padRight(" + describe(text) + ", " + std::to_string(cells) + ") keeps the text");
        }
    }
}

// ---------------------------------------------------------------------------
// 6. formatCount
// ---------------------------------------------------------------------------

void checkFormatCount() {
    struct CountCase {
        long long value;
        const char* text;
    };
    const CountCase cases[] = {
        {0, "0"},
        {1, "1"},
        {999, "999"},
        {1000, "1k"},
        {1049, "1k"},
        {1050, "1.1k"},
        {1234, "1.2k"},
        {1250, "1.3k"},
        {1950, "2k"},
        {1999, "2k"},
        {9999, "10k"},
        {12345, "12k"},
        {42000, "42k"},
        {128000, "128k"},
        {999499, "999k"},
        {999500, "1M"},
        {999999, "1M"},
        {1000000, "1M"},
        {1234567, "1.2M"},
        {1500000, "1.5M"},
        {999999999, "1G"},
        {1000000000, "1G"},
        {1234567890, "1.2G"},
        {2000000000000LL, "2T"},
        {3000000000000000LL, "3P"},
        {4000000000000000000LL, "4E"},
        {-1, "-1"},
        {-999, "-999"},
        {-1234, "-1.2k"},
        {-1000000, "-1M"},
    };
    for (const CountCase& c : cases) {
        expectText(ui::formatCount(c.value), c.text,
                   "formatCount(" + std::to_string(c.value) + ")");
    }

    // The extremes of the type: the magnitude is computed in unsigned, so the
    // smallest value does not overflow into a positive number.
    expectText(ui::formatCount(9223372036854775807LL), "9.2E", "formatCount(LLONG_MAX)");
    expectText(ui::formatCount(-9223372036854775807LL - 1), "-9.2E", "formatCount(LLONG_MIN)");

    // Properties: no "1000k" (a carry promotes the unit), no "10.0k" (the
    // tenths become the unit), ASCII digits only, at most one decimal point.
    // Counted rather than checked per value: the sweep is wide and a verbose run
    // should stay readable.
    int carried = 0;
    int emptyDecimal = 0;
    int foreignChar = 0;
    int extraDots = 0;
    std::string firstBad;
    for (long long value = 0; value < 3000000; value += 7) {
        const std::string text = ui::formatCount(value);
        const bool badCarry = text.rfind("1000", 0) == 0;
        const bool badDecimal = text.find(".0") != std::string::npos;
        int dots = 0;
        bool plain = true;
        for (const char c : text) {
            if (c == '.') {
                ++dots;
            } else if (!((c >= '0' && c <= '9') || c == 'k' || c == 'M' || c == 'G' || c == 'T' ||
                         c == 'P' || c == 'E')) {
                plain = false;
            }
        }
        if (badCarry || badDecimal || !plain || dots > 1) {
            if (firstBad.empty()) {
                firstBad = std::to_string(value) + " -> " + text;
            }
            carried += badCarry ? 1 : 0;
            emptyDecimal += badDecimal ? 1 : 0;
            foreignChar += plain ? 0 : 1;
            extraDots += dots > 1 ? 1 : 0;
        }
    }
    const std::string tail = firstBad.empty() ? std::string() : " (first: " + firstBad + ")";
    check(carried == 0, "formatCount never answers 1000<suffix>" + tail);
    check(emptyDecimal == 0, "formatCount never answers an empty decimal" + tail);
    check(foreignChar == 0, "formatCount is plain digits and one unit letter" + tail);
    check(extraDots == 0, "formatCount never has two decimal points" + tail);
}

// ---------------------------------------------------------------------------
// A helper neither module owns alone: the cut marker follows the glyph table
// ---------------------------------------------------------------------------

/// `truncate(text, cells)` marks its cut with `ui::icon(Icon::Ellipsis)`, so the
/// marker follows the glyph table's own fallback decision instead of a literal in
/// `ui/text.h`. This is the hand-off between the two modules, checked in both modes:
/// a terminal that gets the ASCII column must not be handed a "…" by a truncation.
void checkDefaultMarker() {
    const char* const previous = std::getenv("LLMFUN_ASCII");
    const bool hadPrevious = previous != nullptr;
    const std::string saved = hadPrevious ? previous : std::string();

    const bool modes[2] = {false, true};
    for (const bool asciiMode : modes) {
        ::setenv("LLMFUN_ASCII", asciiMode ? "1" : "0", 1);
        const std::string label =
            std::string("the default marker in ") + (asciiMode ? "ASCII" : "Unicode") + " mode";

        expectText(ui::icon(ui::Icon::Ellipsis), asciiMode ? "..." : "…",
                   label + " is the table's own");
        expectText(ui::truncate("abcdef", 5),
                   ui::truncate("abcdef", 5, ui::icon(ui::Icon::Ellipsis)),
                   label + " is what truncate(text, cells) uses");
        // A marker wider than the budget is itself cut, so a one-cell column still
        // cannot be overrun by the three-dot form.
        expectText(ui::truncate("abcdef", 1), asciiMode ? "." : "…",
                   label + " is itself cut by a one-cell budget");
        for (int cells = 1; cells <= 12; ++cells) {
            const std::string cut = ui::truncate("abcdefghijkl", cells);
            check(ui::displayWidth(cut) <= cells, label + ": a cut to " + std::to_string(cells) +
                                                      " cells stays inside the budget (" +
                                                      describe(cut) + " is " +
                                                      std::to_string(ui::displayWidth(cut)) + ")");
        }
    }

    // Leave the process the way it was found: the checks after this one do not care
    // about the mode, and a later one must not inherit the last iteration's.
    if (hadPrevious)
        ::setenv("LLMFUN_ASCII", saved.c_str(), 1);
    else
        ::unsetenv("LLMFUN_ASCII");
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0 || std::strcmp(argv[i], "--verbose") == 0) {
            g_verbose = true;
        } else {
            std::fprintf(stderr, "usage: %s [-v]\n", argv[0]);
            return 2;
        }
    }

    // truncate()'s default marker comes from the glyph table, so the mode decides
    // what a cut looks like. Pin the glyph column: the expectations below are written
    // against "…", and a run on a linux console or a CJK locale would otherwise fail
    // for a reason that is not this module's. checkDefaultMarker() covers the ASCII
    // side and restores what it found.
    const char* const previousAscii = std::getenv("LLMFUN_ASCII");
    const std::string savedAscii = previousAscii != nullptr ? previousAscii : std::string();
    const bool hadAscii = previousAscii != nullptr;
    ::setenv("LLMFUN_ASCII", "0", 1);

    checkDecoding();
    checkCellTable();
    checkTableSweep();
    checkTruncate();
    checkDefaultMarker();
    checkWrap();
    checkPadRight();
    checkFormatCount();

    if (hadAscii)
        ::setenv("LLMFUN_ASCII", savedAscii.c_str(), 1);
    else
        ::unsetenv("LLMFUN_ASCII");

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("test_text: all checks passed\n");
    return 0;
}
