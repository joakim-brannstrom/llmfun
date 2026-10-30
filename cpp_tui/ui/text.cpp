/// The text/width module (ui/text.h, design §4.15): the cell-width table the
/// ncurses backend renders with, a UTF-8 decoder that never cuts a sequence in
/// half, and the four operations the shell builds on them.
///
/// The table below is a *copy* of `imtui_wcwidth()` and
/// `imtui_is_emoji_presentation()` in `vendor/imgui/imgui/imgui_draw.cpp`. Both
/// are `static` in a vendored translation unit, so there is no call to make, and
/// a shared header would invert the dependency (the renderer would need the
/// app). What keeps the copy honest is a test, not a hope: `test_text` sweeps
/// every codepoint against an independent oracle built from the same reference
/// data, and `imtui_utf8_grid_test` renders a corpus through the real table.
///
/// `truncate()`'s default marker is `ui::icon(Icon::Ellipsis)` (ui/icons.h), so a cut
/// on a terminal that gets the ASCII column ends in "..." instead of a glyph that
/// terminal cannot draw. `icons.h` is a leaf header - it includes nothing - so this
/// module's include surface does not grow.
#include "text.h"

#include "icons.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace llmfun::tui::ui {
namespace {

// ---------------------------------------------------------------------------
// UTF-8 decoding
// ---------------------------------------------------------------------------

/// One decoded codepoint. `bytes` is always at least 1, so a walk always
/// advances; an invalid sequence answers `valid == false` and consumes exactly
/// one byte, which keeps the walk total for arbitrary input.
struct Codepoint {
    uint32_t value = 0;
    int bytes = 1;
    bool valid = false;
};

/// U+FFFD, the substitution character. Only used for its width (1 cell), never
/// written out: measuring must not rewrite the text. It is also what the
/// renderer substitutes (`IM_UNICODE_CODEPOINT_INVALID`).
constexpr uint32_t kReplacement = 0xFFFD;

/// Bytes the sequence starting at `lead` claims, read off its high bits exactly
/// like the renderer's own decoder reads them (`ImTextCharFromUtf8`'s length
/// table): 1 for ASCII and for a byte that cannot lead (a stray continuation,
/// F8..FF), 2 for C0..DF (the two overlong leads included), 3 for E0..EF, 4 for
/// F0..F7 (the out-of-range leads included).
int claimedLength(unsigned char lead) {
    if (lead < 0xC0)
        return 1;
    if (lead < 0xE0)
        return 2;
    if (lead < 0xF0)
        return 3;
    if (lead < 0xF8)
        return 4;
    return 1;
}

Codepoint decodeAt(std::string_view utf8, std::size_t at) {
    const auto byteAt = [&utf8](std::size_t index) {
        return static_cast<unsigned char>(utf8[index]);
    };
    // An invalid sequence consumes the bytes its lead byte claims, capped by what
    // is left and by a zero byte - the renderer's rule (`ImTextCharFromUtf8`
    // returns `wanted` clamped to the available non-zero bytes and substitutes
    // one U+FFFD, which is drawn as one cell). Consuming one byte per bad byte
    // instead would measure a truncated three-byte glyph as three cells where the
    // backend draws one.
    const auto invalid = [&utf8, at](unsigned char lead) {
        std::size_t claimed = static_cast<std::size_t>(claimedLength(lead));
        const std::size_t available = utf8.size() - at;
        if (claimed > available) {
            claimed = available;
        }
        std::size_t consumed = 1;
        while (consumed < claimed && utf8[at + consumed] != '\0') {
            ++consumed;
        }
        return Codepoint{kReplacement, static_cast<int>(consumed), false};
    };

    const unsigned char lead = byteAt(at);
    if (lead < 0x80) {
        return Codepoint{lead, 1, true};
    }

    int continuations = 0;
    uint32_t value = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
        continuations = 1;
        value = lead & 0x1Fu;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        continuations = 2;
        value = lead & 0x0Fu;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        continuations = 3;
        value = lead & 0x07u;
    } else {
        // A continuation byte with no lead, an overlong two-byte lead (C0/C1), a
        // lead past U+10FFFF (F5..F7) or one of the 5/6-byte forms UTF-8 dropped.
        return invalid(lead);
    }

    if (static_cast<std::size_t>(continuations) >= utf8.size() - at) {
        return invalid(lead); // truncated
    }
    for (int i = 1; i <= continuations; ++i) {
        const unsigned char continuation = byteAt(at + static_cast<std::size_t>(i));
        if ((continuation & 0xC0) != 0x80) {
            return invalid(lead);
        }
        value = (value << 6) | (continuation & 0x3Fu);
    }
    // Non-canonical (overlong) encoding, surrogate half, out of range: the same
    // three conditions the renderer's decoder rejects.
    if (value < 0x80 || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        return invalid(lead);
    }
    return Codepoint{value, continuations + 1, true};
}

// ---------------------------------------------------------------------------
// The width table
// ---------------------------------------------------------------------------

/// `LLMFUN_IMTUI_EMOJI_WIDTH=1` forces the emoji-presentation class to one cell
/// on terminals that draw emoji narrow. Read once per process, exactly like the
/// backend reads it: a measure that disagreed with the renderer would misalign
/// every row an emoji appears in, which is the whole point of using this table.
bool emojiWidthOne() {
    static const bool cached = [] {
        const char* const value = std::getenv("LLMFUN_IMTUI_EMOJI_WIDTH");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return cached;
}

/// The Emoji_Presentation=Yes class (emoji-data.txt 15.1, 2023-09) - the
/// codepoints a conforming terminal draws two cells wide without a variation
/// selector. Ranges mirror `imtui_is_emoji_presentation()` exactly, including
/// the deliberately coarse 0x1F000-0x1F644 / 0x1FA70-0x1FAFF blocks.
struct WidthRange {
    uint32_t first;
    uint32_t last;
};

constexpr WidthRange kEmojiPresentation[] = {
    {0x231A, 0x231B},   // watch, hourglass done
    {0x23E9, 0x23F3},   // fast-forward..hourglass with flowing sand
    {0x23F8, 0x23FA},   // double vertical bar..record button
    {0x25FD, 0x25FE},   // white/black medium small square
    {0x2614, 0x2615},   // umbrella with rain drops, hot beverage
    {0x2648, 0x2653},   // Aries..Pisces
    {0x267F, 0x267F},   // wheelchair symbol
    {0x2693, 0x2693},   // anchor
    {0x26A1, 0x26A1},   // high voltage
    {0x26AA, 0x26AB},   // medium white/black circle
    {0x26BD, 0x26BE},   // soccer ball, baseball
    {0x26C4, 0x26C5},   // snowman without snow, sun behind cloud
    {0x26CE, 0x26CE},   // ophiuchus
    {0x26D4, 0x26D4},   // no entry
    {0x26EA, 0x26EA},   // church
    {0x26F2, 0x26F3},   // fountain, flag in hole
    {0x26F5, 0x26F5},   // sailboat
    {0x26FA, 0x26FA},   // tent
    {0x26FD, 0x26FD},   // fuel pump
    {0x2705, 0x2705},   // white heavy check mark
    {0x270A, 0x270B},   // raised fist, raised hand
    {0x2728, 0x2728},   // sparkles
    {0x274C, 0x274C},   // cross mark
    {0x274E, 0x274E},   // negative squared cross mark
    {0x2753, 0x2755},   // question/exclamation ornament marks
    {0x2757, 0x2757},   // heavy exclamation mark
    {0x2795, 0x2797},   // heavy plus/minus/division sign
    {0x27B0, 0x27B0},   // curly loop
    {0x27BF, 0x27BF},   // double curly loop
    {0x2B1B, 0x2B1C},   // black/white large square
    {0x2B50, 0x2B50},   // star
    {0x2B55, 0x2B55},   // heavy large circle
    {0x1F000, 0x1F644}, // mahjong tiles..face with rolling eyes
    {0x1F300, 0x1F6FF}, // cyclone..transport symbols
    {0x1F7E0, 0x1F7EB}, // large colored circles
    {0x1F7F0, 0x1F7F0}, // heavy equals sign
    {0x1F900, 0x1F9FF}, // supplemental symbols and pictographs
    {0x1FA70, 0x1FAFF}, // symbols and pictographs extended-A
};

bool isEmojiPresentation(uint32_t ucs) {
    for (const WidthRange& range : kEmojiPresentation) {
        if (ucs >= range.first && ucs <= range.last) {
            return true;
        }
    }
    return false;
}

/// Cells one codepoint occupies, in the order `imtui_wcwidth()` tests its
/// classes: zero-width first (it wins over every wide range), then the regional
/// indicators, then East Asian Wide/Fullwidth, then the emoji class.
int codepointWidth(uint32_t ucs) {
    // NUL and the C0/C1 controls; NBSP (U+00A0) is a normal one-cell space.
    if (ucs == 0) {
        return 0;
    }
    if (ucs < 32 || (ucs >= 0x7F && ucs < 0xA0)) {
        return 0;
    }

    // Combining marks: they attach to the cell before them.
    if (ucs >= 0x0300 && ucs <= 0x036F)
        return 0;
    if (ucs >= 0x1AB0 && ucs <= 0x1AFF)
        return 0;
    if (ucs >= 0x1DC0 && ucs <= 0x1DFF)
        return 0;
    if (ucs >= 0x20D0 && ucs <= 0x20FF)
        return 0;
    if (ucs >= 0xFE20 && ucs <= 0xFE2F)
        return 0;

    // Variation selectors, including VS16 (U+FE0F).
    if (ucs >= 0xFE00 && ucs <= 0xFE0F)
        return 0;

    // Zero-width format and control blocks.
    if (ucs >= 0x200B && ucs <= 0x200F)
        return 0; // ZWSP, ZWNJ, ZWJ, LRM, RLM
    if (ucs >= 0x2028 && ucs <= 0x202E)
        return 0; // line/paragraph separators, bidi controls
    if (ucs >= 0x2060 && ucs <= 0x206F)
        return 0; // word joiner, invisible operators
    if (ucs == 0xFEFF)
        return 0; // BOM / ZWNBSP
    if (ucs >= 0xFFF9 && ucs <= 0xFFFB)
        return 0; // interlinear annotation anchors

    // One cell each: a flag is a grapheme cluster the grid cannot represent, and
    // this has to win over the coarse 0x1F000-0x1F644 emoji block.
    if (ucs >= 0x1F1E6 && ucs <= 0x1F1FF)
        return 1;

    // East Asian Wide/Fullwidth.
    if (ucs >= 0x1100 && ucs <= 0x115F)
        return 2;
    if (ucs >= 0x2329 && ucs <= 0x232A)
        return 2;
    if (ucs >= 0x2E80 && ucs <= 0xA4CF)
        return 2;
    if (ucs >= 0xA960 && ucs <= 0xA97C)
        return 2;
    if (ucs >= 0xAC00 && ucs <= 0xD7A3)
        return 2;
    if (ucs >= 0xF900 && ucs <= 0xFAFF)
        return 2;
    if (ucs >= 0xFE10 && ucs <= 0xFE19)
        return 2;
    if (ucs >= 0xFE30 && ucs <= 0xFE6F)
        return 2;
    if (ucs >= 0xFF01 && ucs <= 0xFF60)
        return 2;
    if (ucs >= 0xFFE0 && ucs <= 0xFFE6)
        return 2;
    if (ucs >= 0x20000 && ucs <= 0x2FFFD)
        return 2;
    if (ucs >= 0x30000 && ucs <= 0x3FFFD)
        return 2;

    if (isEmojiPresentation(ucs)) {
        return emojiWidthOne() ? 1 : 2;
    }
    return 1;
}

int codepointCells(const Codepoint& codepoint) {
    // An invalid byte measures like U+FFFD: one cell, never zero and never wide.
    return codepoint.valid ? codepointWidth(codepoint.value) : 1;
}

// ---------------------------------------------------------------------------
// Measuring helpers
// ---------------------------------------------------------------------------

/// Bytes of the longest prefix of `utf8` that fits `cells` cells, cut between
/// codepoints. Zero-width marks cost nothing, so they are kept.
std::size_t prefixBytes(std::string_view utf8, int cells) {
    int usedCells = 0;
    std::size_t at = 0;
    while (at < utf8.size()) {
        const Codepoint codepoint = decodeAt(utf8, at);
        const int widthCells = codepointCells(codepoint);
        if (widthCells > 0 && usedCells + widthCells > cells) {
            break;
        }
        usedCells += widthCells;
        at += static_cast<std::size_t>(codepoint.bytes);
    }
    return at;
}

/// Line-break whitespace. ASCII space and tab only: every other run - CJK, an
/// ideographic space, a path - is broken at the cell budget instead, which is
/// what those runs need anyway.
bool isBreakSpace(uint32_t ucs) { return ucs == ' ' || ucs == '\t'; }

/// End of the run of spaces/tabs that ends at `end`, not below `floor`.
std::size_t trimTrailingSpaces(std::string_view line, std::size_t floor, std::size_t end) {
    std::size_t cut = end;
    while (cut > floor) {
        // Walk back one codepoint: spaces and tabs are single bytes, so the
        // byte walk cannot split a sequence here.
        if (!isBreakSpace(static_cast<unsigned char>(line[cut - 1]))) {
            break;
        }
        --cut;
    }
    return cut;
}

/// Append the lines of one paragraph (a run without '\n') to `lines`.
void wrapParagraph(std::string_view paragraph, int cells, std::vector<std::string_view>& lines) {
    if (paragraph.empty()) {
        lines.push_back(paragraph);
        return;
    }

    std::size_t lineStart = 0;
    while (lineStart < paragraph.size()) {
        int usedCells = 0;
        std::size_t drawnEnd = lineStart;
        std::size_t nextStart = paragraph.size();
        std::size_t at = lineStart;

        while (at < paragraph.size()) {
            // One token: a run of break spaces, or a run of everything else.
            const bool spaces = isBreakSpace(decodeAt(paragraph, at).value);
            std::size_t tokenEnd = at;
            int tokenCells = 0;
            while (tokenEnd < paragraph.size()) {
                const Codepoint codepoint = decodeAt(paragraph, tokenEnd);
                if (isBreakSpace(codepoint.value) != spaces) {
                    break;
                }
                tokenCells += codepointCells(codepoint);
                tokenEnd += static_cast<std::size_t>(codepoint.bytes);
                // Stop as soon as a word cannot fit what is left: how long it is
                // does not matter then, and walking the rest of a 64 KB run once
                // per line is quadratic (a long path or minified JSON is one
                // word). Space runs are walked whole - `nextStart` needs their
                // end - and they are consumed in one step, so that stays linear.
                if (!spaces && usedCells + tokenCells > cells) {
                    break;
                }
            }

            if (usedCells + tokenCells <= cells) {
                usedCells += tokenCells;
                drawnEnd = tokenEnd;
                at = tokenEnd;
                continue;
            }

            if (spaces && usedCells > 0) {
                // The gap itself does not fit what is left: the line ends before
                // it and the run is dropped - a break is where the whitespace
                // goes, so it is drawn at neither end.
                nextStart = tokenEnd;
                break;
            }

            if (!spaces && usedCells > 0) {
                const std::size_t trimmed = trimTrailingSpaces(paragraph, lineStart, drawnEnd);
                if (trimmed > lineStart) {
                    // Break in front of the word, without the spaces that were
                    // laid down for the gap.
                    drawnEnd = trimmed;
                    nextStart = at;
                    break;
                }
                // Only the paragraph's indent is on the line so far. The word
                // starts on it when its first glyph fits what is left (a line of
                // nothing would be a lie about the width); otherwise the indent
                // is the line and the word moves down, because the budget is not
                // negotiable - not even for one glyph, which is the exception
                // `usedCells == 0` already covers below.
                const int roomCells = cells - usedCells;
                if (roomCells <= 0 || codepointCells(decodeAt(paragraph, at)) > roomCells) {
                    nextStart = at;
                    break;
                }
            }

            // A hard break is the only way to make progress here: the codepoints
            // that fit the remaining budget, always at least one, because a wide
            // glyph on a one-cell line still has to go somewhere.
            const int budgetCells = cells - usedCells;
            std::size_t hardEnd = at;
            int hardCells = 0;
            while (hardEnd < paragraph.size()) {
                const Codepoint codepoint = decodeAt(paragraph, hardEnd);
                if (isBreakSpace(codepoint.value) != spaces) {
                    break;
                }
                const int widthCells = codepointCells(codepoint);
                if (hardEnd > at && hardCells + widthCells > budgetCells) {
                    break;
                }
                hardCells += widthCells;
                hardEnd += static_cast<std::size_t>(codepoint.bytes);
            }
            drawnEnd = hardEnd;
            nextStart = hardEnd;
            break;
        }

        lines.push_back(paragraph.substr(lineStart, drawnEnd - lineStart));
        lineStart = nextStart;
    }
}

// ---------------------------------------------------------------------------
// Compact numerals
// ---------------------------------------------------------------------------

/// The decimal ladder: 10^3 .. 10^18. `k` is the smallest, so a magnitude below
/// 1000 has no unit at all.
struct CountUnit {
    unsigned long long scale;
    char suffix;
};

constexpr CountUnit kCountUnits[] = {
    {1000ull, 'k'},          {1000000ull, 'M'},          {1000000000ull, 'G'},
    {1000000000000ull, 'T'}, {1000000000000000ull, 'P'}, {1000000000000000000ull, 'E'},
};
constexpr int kCountUnitCount = int(sizeof(kCountUnits) / sizeof(kCountUnits[0]));

/// `magnitude` written in `unit`, or an empty string when rounding carries it
/// into the next larger unit (999.5k round to 1000k, which reads as 1M).
std::string formatInUnit(unsigned long long magnitude, const CountUnit& unit) {
    const unsigned long long whole = magnitude / unit.scale;
    const unsigned long long rest = magnitude % unit.scale;
    if (whole < 10) {
        // One decimal below ten units: 1.2k, 9.9M. A zero tenth is dropped, so
        // a value never reads as 10.0k or 1.0M.
        const unsigned long long tenths = (rest * 10 + unit.scale / 2) / unit.scale;
        if (tenths < 10) {
            std::string text = std::to_string(whole);
            if (tenths != 0) {
                text += '.';
                text += static_cast<char>('0' + static_cast<int>(tenths));
            }
            text += unit.suffix;
            return text;
        }
        // 9.95k rounded up to 10 units: the integer rule below is the answer.
    }
    const unsigned long long rounded = whole + (rest + unit.scale / 2) / unit.scale;
    if (rounded < 1000) {
        return std::to_string(rounded) + unit.suffix;
    }
    return std::string();
}

std::string formatMagnitude(unsigned long long magnitude) {
    int unit = -1;
    for (int i = 0; i < kCountUnitCount; ++i) {
        if (magnitude >= kCountUnits[i].scale) {
            unit = i;
        }
    }
    if (unit < 0) {
        return std::to_string(magnitude);
    }
    std::string text = formatInUnit(magnitude, kCountUnits[unit]);
    if (!text.empty()) {
        return text;
    }
    if (unit + 1 < kCountUnitCount) {
        // A carry is a whole unit of the next rung: 999999 rounds to 1000k,
        // which reads as 1M. The ladder is deep enough that a long long cannot
        // run off its top.
        return std::string("1") + kCountUnits[unit + 1].suffix;
    }
    return std::to_string(magnitude);
}

} // namespace

// ---------------------------------------------------------------------------
// The module
// ---------------------------------------------------------------------------

int displayWidth(std::string_view utf8) {
    int cells = 0;
    std::size_t at = 0;
    while (at < utf8.size()) {
        const Codepoint codepoint = decodeAt(utf8, at);
        cells += codepointCells(codepoint);
        at += static_cast<std::size_t>(codepoint.bytes);
    }
    return cells;
}

std::string truncate(std::string_view utf8, int cells) {
    return truncate(utf8, cells, icon(Icon::Ellipsis));
}

std::string truncate(std::string_view utf8, int cells, const char* ellipsis) {
    if (cells <= 0) {
        return std::string();
    }
    if (displayWidth(utf8) <= cells) {
        return std::string(utf8);
    }
    const std::string_view marker =
        ellipsis != nullptr ? std::string_view(ellipsis) : std::string_view();
    const int markerCells = displayWidth(marker);
    if (markerCells > cells) {
        return std::string(marker.substr(0, prefixBytes(marker, cells)));
    }
    std::string text(utf8.substr(0, prefixBytes(utf8, cells - markerCells)));
    text += marker;
    return text;
}

std::vector<std::string_view> wrap(std::string_view utf8, int cells) {
    std::vector<std::string_view> lines;
    if (cells <= 0) {
        return lines;
    }
    std::size_t start = 0;
    for (;;) {
        const std::size_t end = utf8.find('\n', start);
        const std::size_t length =
            end == std::string_view::npos ? std::string_view::npos : end - start;
        wrapParagraph(utf8.substr(start, length), cells, lines);
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return lines;
}

std::string padRight(std::string_view utf8, int cells) {
    std::string text(utf8);
    const int widthCells = displayWidth(utf8);
    if (widthCells < cells) {
        text.append(static_cast<std::size_t>(cells - widthCells), ' ');
    }
    return text;
}

std::string formatCount(long long v) {
    if (v < 0) {
        // Negate in unsigned: -LLONG_MIN has no signed representation, and the
        // magnitude of the smallest value is exactly its two's complement.
        const unsigned long long magnitude = ~static_cast<unsigned long long>(v) + 1ull;
        return std::string("-") + formatMagnitude(magnitude);
    }
    return formatMagnitude(static_cast<unsigned long long>(v));
}

int utf8Count(std::string_view utf8) {
    int count = 0;
    for (const char c : utf8) {
        // A continuation byte (`10xxxxxx`) belongs to the codepoint its lead byte opened.
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80)
            ++count;
    }
    return count;
}

int estimateWrappedLines(std::string_view utf8) {
    if (utf8.empty())
        return 0;
    int lines = 0;
    std::size_t start = 0;
    while (true) {
        const std::size_t newline = utf8.find('\n', start);
        const std::size_t end = newline == std::string_view::npos ? utf8.size() : newline;
        const std::size_t length = end - start;
        // ceil(length / reference width), and one line even when the line is empty:
        // exactly 80 columns is one line, not two.
        const std::size_t wrapped = (length + kEstimateColumns - 1) / kEstimateColumns;
        lines += static_cast<int>(std::max<std::size_t>(1, wrapped));
        if (newline == std::string_view::npos)
            break;
        start = newline + 1;
    }
    return lines;
}

} // namespace llmfun::tui::ui
