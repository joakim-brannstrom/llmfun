// session_fuzzy.h
//
// Pure, self-contained fzf-style fuzzy matcher for session titles
// (Phase 4 item 2, design A20/A21/A27; plan/implementation_plan.md Task 1).
//
// The public interface is a pure function:
//
//     int fuzzyScore(const std::string& query, const std::string& text);
//
// a multi-field convenience built on it (Phase 4 item 2 Task 9, R26):
//
//     int fuzzyScoreFields(const std::string& query,
//                          const std::string& title,
//                          const std::string& preview);
//
// which matches a session against both its title and preview (title weighted
// 2x; a session matches if either field matches), and a match-position
// companion (Phase 4 item 2 Task 10, R25) built on the same alignment:
//
//     bool fuzzyMatchPositions(const std::string& query,
//                              const std::string& text,
//                              std::vector<std::size_t>& positions);
//
// which fills `positions` with the byte offsets of the matched text bytes so
// the TUI can highlight the matched characters (R25).
//
// Returns -1 if `query` is not a case-insensitive byte-level subsequence of
// `text`; otherwise the match score clamped to a floor of 0 (A27), where a
// higher score means a better match (A21). A boolean "does it match" is
// subsumed by `fuzzyScore(...) >= 0`. The scoring functions are pure and
// total: no allocation, no throw, no out-of-bounds read (`fuzzyMatchPositions`
// additionally fills a caller-owned, reusable vector, N5). `query` and
// `text` are UTF-8 byte strings;
// matching is byte-level (N7), so a multi-byte character matches as its exact
// byte sequence and is never split. The reported match positions are byte
// offsets; for valid UTF-8 input they always fall on character boundaries
// (a multi-byte character matches only as its whole byte sequence). The
// renderer snaps highlight run boundaries to character starts and extends
// each run to the whole word enclosing it, so a character is never split
// (N7/R25).
//
// Matching (A20): case-insensitive byte-level subsequence. Each query byte
// must appear in `text` in order (not necessarily contiguously). Case-folding
// is ASCII-only (explicit A-Z fold, locale-independent). Multi-byte
// (non-ASCII) bytes are NOT case-folded — they match as exact byte sequences.
// This is UTF-8 safe: ASCII bytes (0x00-0x7F) are unambiguous in UTF-8, so an
// ASCII query byte can never match a continuation byte of a multi-byte
// character, and a multi-byte query character only matches the identical
// character.
//
// Scoring (A21, simplified fzf): computed on the LEFTMOST subsequence
// alignment (the first valid alignment scanning left-to-right):
//     +100  per matched query byte (base)
//     +40   if the matched byte is at a word boundary (start of text, or
//           immediately after a space / '-' / '_' / '/')
//     +25   if the match is consecutive with the previous matched byte
//           (adjacent text positions)
//     -3    per skipped byte between consecutive matches (gap penalty)
//     -1    per position of the first matched byte (earlier first match =
//           better)
// The raw score can be NEGATIVE for a valid-but-poor match (the gap and
// first-position penalties are both unbounded by title length), so the
// returned match score is clamped to a floor of 0 (A27): -1 = no match,
// >= 0 = match. Clamping only affects very-poor matches, which all tie at 0
// and fall back to the caller's stable snapshot order.
//
// The weights are a tunable baseline (A21); they are kept as named constants
// at the top of this header so the P3 DP scoring (Task 15) reuses the same
// factors.
//
// No imtui/ncurses/ImGui dependency: this header is standalone so it unit-
// tests without the TUI (see test_session_fuzzy.cpp).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace llmfun::tui {

// Score weights (A21 baseline, tunable). Reused by the P3 DP scoring
// (Task 15).
inline constexpr int kFuzzyBase = 100;       // per matched query byte
inline constexpr int kFuzzyBoundary = 40;    // matched byte at a word boundary
inline constexpr int kFuzzyConsecutive = 25; // adjacent to the previous match
inline constexpr int kFuzzyGap = 3;          // per skipped byte between matches
inline constexpr int kFuzzyFirstPos = 1;     // per position of the first match

// Case-fold one byte (ASCII-only, A20). Non-ASCII (multi-byte) bytes pass
// through unchanged, so they match as exact byte sequences. Deliberately NOT
// std::tolower: tolower is locale-dependent (C standard) and the TUI calls
// setlocale(LC_ALL, "") (tui.cpp tuiInit); in a single-byte locale (e.g.
// en_US.iso8859-1) it would fold 0x80-0xFF and break the "multi-byte never
// folded" contract (N7). This explicit A-Z fold is locale-independent and
// correct-by-construction.
static unsigned char fuzzyFold(unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<unsigned char>(c - 'A' + 'a');
    }
    return c;
}

// True if text position `pos` is a word boundary (A21): start of text, or
// immediately after a space / '-' / '_' / '/'. The boundary characters are
// ASCII and unaffected by case-folding, so the original (un-folded) byte is
// used.
static bool fuzzyIsBoundary(const std::string& text, std::size_t pos) {
    if (pos == 0) {
        return true;
    }
    const char c = text[pos - 1];
    return c == ' ' || c == '-' || c == '_' || c == '/';
}

// Leftmost subsequence alignment (A21): match each query byte to the
// EARLIEST text position at/after the previous match (one left-to-right
// pass, no allocation). Returns false when the query is not a subsequence of
// the text. On success, *raw receives the UN-clamped score (the public
// functions clamp to the floor of 0 per A27) and, when non-null, *positions
// receives the text byte offset of each matched query byte in match order
// (cleared first; the caller owns the vector and may reuse it). Assumes a
// non-empty query no longer than the text; the public functions apply those
// guards.
static bool fuzzyAlign(const std::string& query, const std::string& text, std::int64_t* raw,
                       std::vector<std::size_t>* positions) {
    if (positions != nullptr) {
        positions->clear();
    }

    const std::size_t nQ = query.size();
    const std::size_t nT = text.size();
    if (nQ > nT) {
        return false; // a subsequence can never be longer than its text
    }

    std::int64_t score = 0;
    std::size_t scan = 0; // text cursor: earliest position for the next byte
    int prevPos = -1;     // text position of the previous matched byte
    int firstPos = -1;    // text position of the first matched byte

    for (std::size_t qi = 0; qi < nQ; ++qi) {
        const unsigned char q = fuzzyFold(static_cast<unsigned char>(query[qi]));
        std::size_t found = nT;
        for (std::size_t p = scan; p < nT; ++p) {
            if (fuzzyFold(static_cast<unsigned char>(text[p])) == q) {
                found = p;
                break;
            }
        }
        if (found == nT) {
            return false; // this query byte has no match -> not a subsequence
        }
        if (positions != nullptr) {
            positions->push_back(found);
        }
        if (firstPos < 0) {
            firstPos = static_cast<int>(found);
        }
        score += kFuzzyBase;
        if (fuzzyIsBoundary(text, found)) {
            score += kFuzzyBoundary;
        }
        if (prevPos >= 0) {
            const long long gap = static_cast<long long>(found) - prevPos - 1;
            if (gap == 0) {
                score += kFuzzyConsecutive;
            } else {
                score -= static_cast<long long>(kFuzzyGap) * gap;
            }
        }
        prevPos = static_cast<int>(found);
        scan = found + 1;
    }

    score -= static_cast<long long>(kFuzzyFirstPos) * firstPos;
    *raw = score;
    return true;
}

// See the header comment for the full contract.
inline int fuzzyScore(const std::string& query, const std::string& text) {
    // Degenerate "no filter": the caller treats an empty query as "show all"
    // in snapshot order, so an empty query scores 0 (a match) rather than -1
    // (A20).
    if (query.empty()) {
        return 0;
    }

    const std::size_t nT = text.size();
    if (query.size() > nT) {
        return -1; // a subsequence can never be longer than its text
    }

    std::int64_t raw = 0;
    if (!fuzzyAlign(query, text, &raw, nullptr)) {
        return -1; // not a subsequence
    }
    if (raw < 0) {
        raw = 0; // clamp to a floor of 0 (A27)
    }
    return static_cast<int>(raw);
}

// Matched byte positions (Phase 4 item 2 Task 10, R25): fill `positions`
// (cleared first) with the text byte offset of each matched query byte in
// match order, using the LEFTMOST alignment — the same alignment fuzzyScore
// scores. Returns true when the query matches (positions holds one entry per
// matched byte), false when it does not (positions left empty). An empty
// query returns false with an empty vector: "no filter" highlights nothing.
// Pure like fuzzyScore except for the caller-owned output vector (reusable,
// so per-frame filtering causes no allocation churn, N5).
inline bool fuzzyMatchPositions(const std::string& query, const std::string& text,
                                std::vector<std::size_t>& positions) {
    positions.clear();
    if (query.empty()) {
        return false; // degenerate "no filter": nothing to highlight
    }
    if (query.size() > text.size()) {
        return false; // a subsequence can never be longer than its text
    }
    std::int64_t raw = 0;
    return fuzzyAlign(query, text, &raw, &positions);
}

// Multi-field score (Phase 4 item 2 Task 9, design R26): match a session
// against BOTH its title and its preview. A session matches if EITHER field
// matches (i.e. either single-field fuzzyScore is >= 0). The combined score
// weights the title 2x over the preview:
//
//     score = max(titleScore, previewScore * 0.5)
//
// where each field's score is its own clamped fuzzyScore (>= 0) when that
// field matches, and is treated as 0 when that field does not match (so a
// non-matching field never contributes). Consequences:
//   - a title match always outranks an equal-strength preview match (the
//     preview score is halved);
//   - a preview-only match (title does not match) contributes previewScore/2
//     and therefore ranks below a comparable title match;
//   - a match on neither field returns -1 (no match).
//
// This keeps the single-field fuzzyScore as the primitive (reused by the P3
// DP scoring, Task 15) and layers the field weighting on top. Pure and total
// like fuzzyScore: no allocation, no throw, no out-of-bounds read.
inline int fuzzyScoreFields(const std::string& query, const std::string& title,
                            const std::string& preview) {
    // Empty query is the degenerate "no filter" case (A20): the caller treats
    // it as "show all", so score it 0 (a match), mirroring fuzzyScore.
    if (query.empty()) {
        return 0;
    }
    const int titleScore = fuzzyScore(query, title);
    const int previewScore = fuzzyScore(query, preview);
    if (titleScore < 0 && previewScore < 0) {
        return -1; // no field matches
    }
    // Title weighted 2x (R26): a matching preview contributes half its raw
    // score; a non-matching field contributes 0. Integer division (floor) is
    // fine for ranking (ties fall back to stable snapshot order).
    const int titlePart = (titleScore >= 0) ? titleScore : 0;
    const int previewPart = (previewScore >= 0) ? (previewScore / 2) : 0;
    return titlePart > previewPart ? titlePart : previewPart;
}

} // namespace llmfun::tui
