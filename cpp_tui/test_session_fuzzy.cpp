// test_session_fuzzy.cpp
//
// Standalone unit test for the session fuzzy matcher (session_fuzzy.h),
// Phase 4 item 2 Task 1 (plan/implementation_plan.md).
//
// No test framework: a CHECK(cond) macro prints each check and sets a fail
// flag. Exits 0 if every check passes, non-zero otherwise. Links only the
// standard library (no imtui/ncurses/ImGui), which also verifies that
// session_fuzzy.h compiles standalone (no TUI dependency).
//
// Covers (per the task acceptance criteria):
//   - match vs no-match
//   - case-insensitivity
//   - non-contiguous subsequence matching
//   - empty query
//   - the scoring ORDER: earlier > later, consecutive > gapped,
//     word-boundary > mid-word (each a dedicated pair, expected one strictly
//     higher), plus every boundary character (space / '-' / '_' / '/')
//   - multi-byte titles match on exact byte sequences and are never split
//     (incl. a partial byte sequence embedded in a longer text) and never
//     case-folded (an accented char never matches its ASCII lookalike)
//   - a definite no-match returns exactly -1
//   - a poor match on a long title (large gap) returns 0 (clamped, A27)
//   - edge cases: empty text, query longer than text
//   - exact score pins (guard against weight regressions in Task 15)
//   - match positions (Task 10, R25): leftmost byte offsets, one per matched
//     query byte, vector cleared/reused across calls, UTF-8 boundary safety

#include <cstdio>
#include <string>

#include "session_fuzzy.h"

using llmfun::tui::fuzzyMatchPositions;
using llmfun::tui::fuzzyScore;
using llmfun::tui::fuzzyScoreFields;
static int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        const bool _ok = (cond);                                                                   \
        std::printf("%s %s\n", _ok ? "ok  " : "FAIL", #cond);                                      \
        if (!_ok) {                                                                                \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

int main() {
    // --- match vs no-match ------------------------------------------------
    CHECK(fuzzyScore("ab", "xabcy") >= 0);  // 'a' (1), 'b' (2) in order
    CHECK(fuzzyScore("ba", "xabcy") == -1); // 'b' before 'a': no subsequence
    CHECK(fuzzyScore("xyz", "abc") == -1);  // no ordered common bytes

    // --- case-insensitivity -----------------------------------------------
    CHECK(fuzzyScore("ABC", "abc") >= 0);
    CHECK(fuzzyScore("AbC", "aBc") >= 0);
    // Same alignment, case folded away -> identical score.
    CHECK(fuzzyScore("ABC", "abc") == fuzzyScore("abc", "abc"));

    // --- non-contiguous subsequence matching --------------------------------
    CHECK(fuzzyScore("ac", "abc") >= 0);     // 'a' (0), 'c' (2), skip 'b'
    CHECK(fuzzyScore("ace", "abcdef") >= 0); // a (0), c (2), e (4), in order

    // --- empty query -------------------------------------------------------
    CHECK(fuzzyScore("", "anything") == 0);
    CHECK(fuzzyScore("", "") == 0);

    // --- scoring ORDER (dedicated pairs; expected one strictly higher) -----
    // earlier > later: both 't' sit at a word boundary (preceded by a space);
    // only the first position differs (penalty -2 vs -3).
    CHECK(fuzzyScore("t", "a t") > fuzzyScore("t", "ab t"));
    // consecutive > gapped: both start at position 0 (same boundary); 'ab' is
    // adjacent (+25) vs 'aXb' gapped by one byte (-3).
    CHECK(fuzzyScore("ab", "ab") > fuzzyScore("ab", "aXb"));
    // word-boundary > mid-word: 'a' at start of text (boundary, +40) vs 'a'
    // mid-word (no boundary).
    CHECK(fuzzyScore("a", "a") > fuzzyScore("a", "xa"));
    // Every boundary character (space / '-' / '_' / '/') outranks a mid-word
    // match at the same position (boundary +40 vs none).
    CHECK(fuzzyScore("b", "a b") > fuzzyScore("b", "axb")); // space
    CHECK(fuzzyScore("b", "a-b") > fuzzyScore("b", "axb")); // '-'
    CHECK(fuzzyScore("b", "a_b") > fuzzyScore("b", "axb")); // '_'
    CHECK(fuzzyScore("b", "a/b") > fuzzyScore("b", "axb")); // '/'

    // --- multi-byte titles: exact byte sequences, never split --------------
    // "中" is U+4E2D, the UTF-8 bytes 0xE4 0xB8 0xAD.
    CHECK(fuzzyScore("中", "中") >= 0);    // full char matches
    CHECK(fuzzyScore("中", "x中y") >= 0);  // char in context
    CHECK(fuzzyScore("中", "中中") >= 0);  // leftmost of repeats
    CHECK(fuzzyScore("a中", "xa中") >= 0); // ASCII + CJK mixed
    CHECK(fuzzyScore("中", "abc") == -1);  // char bytes absent
    // An incomplete byte sequence (2 of the 3 bytes of "中") cannot match the
    // full character: the query is longer than the text -> no match.
    CHECK(fuzzyScore("中", std::string("\xE4\xB8", 2)) == -1);
    // Stronger: the partial bytes embedded in a LONGER text force the matching
    // loop to run (not just the length shortcut): 0xE4 and 0xB8 match, but the
    // third byte 0xAD is absent -> no match (the character is never "split").
    CHECK(fuzzyScore("中", std::string("x\xE4\xB8y", 4)) == -1);
    // Multi-byte bytes are NOT case-folded: an accented Latin "é" (0xC3 0xA9)
    // matches only itself, exactly, and never its ASCII lookalike 'e' (0x65).
    CHECK(fuzzyScore("é", "é") >= 0);
    CHECK(fuzzyScore("é", "aéb") >= 0);
    CHECK(fuzzyScore("é", "e") == -1); // 0xC3 0xA9 != 0x65 (length shortcut)
    CHECK(fuzzyScore("e", "é") == -1); // ASCII 'e' not in {0xC3,0xA9} (loop)

    // --- definite no-match returns exactly -1 ------------------------------
    CHECK(fuzzyScore("zzz", "abc") == -1);
    CHECK(fuzzyScore("abc", "ab") == -1); // query longer than text, no match

    // --- poor match on a long title (large gap) clamps to 0 (A27) ---------
    // "ab" against "a" + 100 filler bytes + "b": the gap penalty (-3 * 100)
    // drives the raw score negative, so the clamped result is exactly 0
    // (still a match, >= 0) rather than a negative value.
    const std::string longTitle = "a" + std::string(100, 'x') + "b";
    CHECK(fuzzyScore("ab", longTitle) == 0);
    // A tighter gap stays positive (sanity: clamping only affects poor matches).
    CHECK(fuzzyScore("ab", "a" + std::string(5, 'x') + "b") > 0);

    // --- edge cases --------------------------------------------------------
    CHECK(fuzzyScore("a", "") == -1);      // empty text, non-empty query
    CHECK(fuzzyScore("abcd", "ab") == -1); // query longer than text

    // --- exact score pins (guard against weight regressions, Task 15) -----
    // These pin the baseline weights (base 100 / boundary 40 / consecutive 25
    // / gap 3 / first-position 1, A21). If a weight changes, these fail, so
    // the P3 DP scoring (Task 15) must update them deliberately.
    CHECK(fuzzyScore("ab", "ab") == 265); // 2*100 + 40(boundary) + 25(consec)
    CHECK(fuzzyScore("a", "a") == 140);   // 100 + 40(boundary, start of text)
    CHECK(fuzzyScore("t", "a t") == 138); // 100 + 40(boundary) - 2(first pos)

    // --- multi-field matching (Task 9, R26: title weighted 2x over preview) --
    // "alpha refactor" and "alpha deploy" both start with "alpha" at a word
    // boundary, so fuzzyScore("alpha", <either>) == 640 (5*100 + 40 + 4*25).
    // A title that lacks 'a' (e.g. "deploy") does not match "alpha" at all.
    CHECK(fuzzyScore("alpha", "alpha refactor") == 640);
    CHECK(fuzzyScore("alpha", "deploy") == -1);

    // Title-only: query matches the title, not the preview -> the combined
    // score is the FULL title score (no halving; the non-matching preview
    // contributes 0).
    CHECK(fuzzyScoreFields("alpha", "alpha refactor", "deploy") ==
          fuzzyScore("alpha", "alpha refactor"));

    // Preview-only: query matches the preview, not the title -> the combined
    // score is HALF the preview score (title weighted 2x).
    CHECK(fuzzyScoreFields("alpha", "deploy", "alpha refactor") ==
          fuzzyScore("alpha", "alpha refactor") / 2);

    // Both: query matches both fields; the title match wins (its full score
    // outranks the halved preview score), so the combined score equals the
    // title score and is strictly greater than the halved preview score.
    const int bothTitle = fuzzyScore("alpha", "alpha refactor");
    const int bothPreview = fuzzyScore("alpha", "alpha deploy");
    CHECK(fuzzyScoreFields("alpha", "alpha refactor", "alpha deploy") == bothTitle);
    CHECK(bothTitle > bothPreview / 2);

    // M1: a preview-only match that is so poor it clamps to 0 must still be a
    // MATCH (0, shown), not a no-match (-1, hidden): the title fails, the
    // preview clamps to 0 -> max(0, 0) == 0 (not both < 0).
    const std::string poorPreview = "a" + std::string(100, 'x') + "b"; // clamps to 0
    CHECK(fuzzyScoreFields("ab", "zzz", poorPreview) == 0);

    // M2: both match, but the title match is so poor it clamps to 0 while the
    // preview is strong: the combined score is the halved preview (the
    // preview part wins the max) and is itself not clamped.
    const std::string weakTitle = "a" + std::string(100, 'x') + "b"; // clamps to 0
    CHECK(fuzzyScoreFields("ab", weakTitle, "ab") == fuzzyScore("ab", "ab") / 2);
    CHECK(fuzzyScoreFields("ab", weakTitle, "ab") > 0); // not itself clamped

    // Ordering: a title-only match ranks strictly above a preview-only match
    // of the same query.
    CHECK(fuzzyScoreFields("alpha", "alpha refactor", "deploy") >
          fuzzyScoreFields("alpha", "deploy", "alpha refactor"));

    // Neither: query matches neither field -> no match (-1).
    CHECK(fuzzyScoreFields("zebra", "deploy", "release") == -1);

    // Degenerate: empty query scores 0 (a match), mirroring fuzzyScore.
    CHECK(fuzzyScoreFields("", "anything", "else") == 0);

    // --- Match positions (Task 10, R25): byte offsets of the matched text
    // --- bytes on the leftmost alignment, one entry per matched query byte.
    {
        std::vector<std::size_t> pos;

        // Empty query: "no filter" -> nothing to highlight, false + empty.
        CHECK(!fuzzyMatchPositions("", "anything", pos));
        CHECK(pos.empty());

        // No match: false + empty (and the vector is left cleared).
        pos.push_back(7);
        CHECK(!fuzzyMatchPositions("z", "abc", pos));
        CHECK(pos.empty());

        // Query longer than text: false + empty.
        CHECK(!fuzzyMatchPositions("abc", "ab", pos));
        CHECK(pos.empty());

        // Consecutive match: one position per matched byte.
        CHECK(fuzzyMatchPositions("ab", "ab", pos));
        CHECK(pos.size() == 2);
        if (pos.size() == 2) {
            CHECK(pos[0] == 0);
            CHECK(pos[1] == 1);
        }

        // Gapped match: positions follow the leftmost alignment
        // ("ab" in "aXXXb" -> 0 and 4).
        CHECK(fuzzyMatchPositions("ab", "aXXXb", pos));
        CHECK(pos.size() == 2);
        if (pos.size() == 2) {
            CHECK(pos[0] == 0);
            CHECK(pos[1] == 4);
        }

        // Case-insensitive: positions honor the same case folding.
        CHECK(fuzzyMatchPositions("AB", "aXXXb", pos));
        CHECK(pos.size() == 2);
        if (pos.size() == 2) {
            CHECK(pos[0] == 0);
            CHECK(pos[1] == 4);
        }

        // Leftmost alignment: "b" in "bxb" -> 0 (not 2).
        CHECK(fuzzyMatchPositions("b", "bxb", pos));
        CHECK(pos.size() == 1 && pos[0] == 0);

        // Consistency with fuzzyScore: the match boolean is identical (a
        // positions result exists iff fuzzyScore reports a match).
        CHECK(fuzzyMatchPositions("ab", "zzzz", pos) == (fuzzyScore("ab", "zzzz") >= 0));
        CHECK(fuzzyMatchPositions("zz", "zz", pos) == (fuzzyScore("zz", "zz") >= 0));

        // Reuse: the vector is cleared at entry, so the caller can reuse one
        // buffer across frames (N5, no allocation churn).
        pos.push_back(99);
        CHECK(fuzzyMatchPositions("a", "ba", pos));
        CHECK(pos.size() == 1 && pos[0] == 1);

        // Multi-byte (UTF-8): each byte of a multi-byte character gets its
        // own position and they fall inside the character's byte range, so a
        // highlighter snapping to character starts can never split it (N7).
        // "café" bytes: c(0) a(1) f(2) é(3,4). Query "cé" matches c(0),
        // then é's two bytes (3,4).
        CHECK(fuzzyMatchPositions("cé", "café", pos));
        CHECK(pos.size() == 3);
        if (pos.size() == 3) {
            CHECK(pos[0] == 0);
            CHECK(pos[1] == 3); // first byte of é
            CHECK(pos[2] == 4); // second byte of é
        }
    }

    std::printf("%s (%d failure%s)\n", g_failures == 0 ? "ALL CHECKS PASSED" : "CHECKS FAILED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
