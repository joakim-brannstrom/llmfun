// test_utf8_grid.cpp
//
// Headless grid-invariant regression test for the imtui UTF-8 cell-width fix
// (implementation plan Task 3; plan/system_design.md §7 Task 3).
//
// This TU renders fixed text rows through the REAL vendored imgui + imtui text
// backend (linked like llmfun_tui: imgui-for-imtui is compiled with IMTUI +
// IMGUI_USE_WCHAR32 by the build; this TU itself defines neither) into an
// ImTui::TScreen grid and asserts the grid-vs-terminal-width invariants that
// Tasks 1-2 restored. It is NOT the fake-ImGui stub used by
// test_inline_code_runtime.cpp.
//
// Setup mirrors cpp_tui/tui.cpp (tuiInit 606-631, renderTabChat 726-734):
//   ImGui::CreateContext(); ImTui_ImplText_Init(); io.DisplaySize = (80,24);
//   per frame: ImTui_ImplText_NewFrame(); ImGui::NewFrame();
//   ImGui::SetCursorPos(0,0); BeginChild("llm_output", (70,8), false,
//   ImGuiWindowFlags_HorizontalScrollbar); <rows>; EndChild();
//   ImGui::Render(); ImTui_ImplText_RenderDrawData(GetDrawData(), &screen);
// Several frames are rendered because the child's vertical scrollbar state is
// computed from the previous frame's content size; assertions run on the last
// frame's grid.
//
// Fixture: 12 rows (mirroring the bug report's shift pattern 0,0,+1,+1,-1,+1):
//   row 0: "a✅b"            probe row for assertion (b)
//   row 1: "e<U+0301><U+200D>b"  combining mark + ZWJ (both width 0)
//   rows 2-7: the six markdown-table rows (2 plain, 3 with ✅, 1 with ❓,
//             1 with ⚠️, 1 with ✅)
//   rows 8-11: plain filler so content (12 rows) exceeds the 8-row child and a
//              vertical scrollbar appears.
//
// Assertions:
//   (a) per-row grid extent (rightmost text cell) equals
//       base_col + oracle_sum(row) - 1, where oracle_sum comes from an
//       INDEPENDENT width oracle (adapted Kuhn wcwidth + the pinned
//       emoji-data.txt 15.1 emoji-presentation list — NOT a call into
//       ImFontIMTuiCellWidth, so the check is not self-referential). Fails
//       pre-fix: ✅/❓ rows are one cell short, the ⚠️ row one cell long.
//   (b) the cell after a wide emoji lands at base_cell + 2: in "a✅b" the 'b'
//       must be at 'a' column + 3, the ✅ cell must report chwidth == 2, and
//       the wide character's second column must be empty. Fails pre-fix
//       ('b' lands at 'a' + 2).
//   (c) no zero-width codepoint from the corpus (U+FE0F, U+200D, U+0301)
//       appears as any cell's ch, every non-space text cell has
//       chwidth >= 1, and ch2 holds only an allowed continuation (0, VS16
//       or a combining mark — never ZWJ) (post-fix guards against width-0
//       leakage). Default mode additionally requires ch2 == 0 everywhere
//       (nothing folds when LLMFUN_IMTUI_EMOJI_PRESENTATION is unset).
//   (d) scrollbar cells (rect cells: ch == ' ', chwidth == 0, non-default bg)
//       occupy one constant column across all six table rows (post-fix guard
//       only — passes pre-fix by construction: the TScreen grid is internally
//       consistent even when broken; only the terminal stream diverges, which
//       Task 4's PTY capture observes).
//   (e) content cells never overwrite the scrollbar column (post-fix guard
//       only; text is clipped to clip_rect.z - 1 by the backend).
//   (f) P3 Task 6 folding mode only (LLMFUN_IMTUI_EMOJI_PRESENTATION=1,
//       set by build_test.py's second run): row 1's 'e' cell carries
//       ch2 == U+0301 (combining mark merged, width unchanged) and row 6's
//       U+26A0 cell carries ch2 == U+FE0F with chwidth == 2 (VS16 promotes
//       the text-default base to emoji presentation); the per-row extents
//       then follow the folding-aware oracle sums.
//
// Exit code 0 only if every check passes. Headless: no ncurses, no PTY; safe
// for CI. Built by llmfun/cpp_tui/CMakeLists.txt (target
// imtui_utf8_grid_test) and executed by vendor/imtui/test/build_test.py
// (twice: default and folding mode).

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

#include "imtui/imtui.h"

// P3 (Task 6) folding mode: build_test.py runs this binary twice — without
// LLMFUN_IMTUI_EMOJI_PRESENTATION (P0 fallback expectations: nothing folds,
// ch2 == 0 everywhere) and with =1 (folding expectations: VS16 promotes the
// U+26A0 base to width 2 with ch2 = U+FE0F, combining marks merge into their
// base). Mirrors the production gate read by imgui_draw.cpp.
static bool g_folding = []() {
    const char * e = getenv("LLMFUN_IMTUI_EMOJI_PRESENTATION");
    return (e != NULL && strcmp(e, "1") == 0);
}();

// ---------------------------------------------------------------------------
// Independent width oracle.
//
// Deliberately NOT ImFontIMTuiCellWidth: same reference data (so the grid
// expectations match conforming terminals), different implementation (a flat
// ordered range table with linear scan). Reference data:
//  - zero-width ranges: Markus Kuhn's wcwidth() non-printing classes plus the
//    format/control block documented in plan/system_design.md §5;
//  - width-2 emoji-presentation ranges: Unicode emoji-data.txt version 15.1
//    (2023-09), the version pinned in imgui_draw.cpp;
//  - U+26A0 (WARNING SIGN) is width 1 (not Emoji_Presentation=Yes; narrow
//    unless VS16 selects emoji presentation);
//  - regional indicators U+1F1E6-U+1F1FF are width 1 each (flag pairs are a
//    documented grapheme-cluster limitation).
// ---------------------------------------------------------------------------

struct WidthRange
{
    uint32_t lo;
    uint32_t hi;
    int      w;
};

// Sorted, non-overlapping; scanned linearly (fixture is tiny).
static const WidthRange kZeroWidth[] =
{
    { 0x0000, 0x0000, 0 },  // NUL
    { 0x0001, 0x001F, 0 },  // control characters
    { 0x007F, 0x009F, 0 },  // DEL.. (Kuhn control rule: ucs >= 0x7F && ucs < 0xA0; NBSP U+00A0 is width 1)
    { 0x0300, 0x036F, 0 },  // combining diacritical marks
    { 0x1AB0, 0x1AFF, 0 },  // combining diacritical marks extended
    { 0x1DC0, 0x1DFF, 0 },  // combining diacritical marks supplement
    { 0x200B, 0x200F, 0 },  // ZWSP, ZWNJ, ZWJ (U+200D), LRM, RLM
    { 0x2028, 0x202E, 0 },  // line/paragraph separators, bidi controls
    { 0x2060, 0x206F, 0 },  // word joiner, invisible operators
    { 0x20D0, 0x20FF, 0 },  // combining marks for symbols
    { 0xFE00, 0xFE0F, 0 },  // variation selectors (incl. VS16 U+FE0F)
    { 0xFE20, 0xFE2F, 0 },  // combining half marks
    { 0xFEFF, 0xFEFF, 0 },  // BOM / ZWNBSP
    { 0xFFF9, 0xFFFB, 0 },  // interlinear annotation anchors
};

static const WidthRange kWide[] =
{
    { 0x1100, 0x115F, 2 },  // Hangul Jamo
    { 0x231A, 0x231B, 2 },  // watch, hourglass done (emoji presentation)
    { 0x2329, 0x232A, 2 },  // angle brackets
    { 0x23E9, 0x23F3, 2 },  // fast-forward..hourglass (emoji presentation)
    { 0x23F8, 0x23FA, 2 },  // double vertical bar..record button
    { 0x25FD, 0x25FE, 2 },  // medium small squares
    { 0x2614, 0x2615, 2 },  // umbrella, hot beverage
    { 0x2648, 0x2653, 2 },  // zodiac signs
    { 0x267F, 0x267F, 2 },  // wheelchair
    { 0x2693, 0x2693, 2 },  // anchor
    { 0x26A1, 0x26A1, 2 },  // high voltage
    { 0x26AA, 0x26AB, 2 },  // medium circles
    { 0x26BD, 0x26BE, 2 },  // soccer, baseball
    { 0x26C4, 0x26C5, 2 },  // snowman, sun behind cloud
    { 0x26CE, 0x26CE, 2 },  // ophiuchus
    { 0x26D4, 0x26D4, 2 },  // no entry
    { 0x26EA, 0x26EA, 2 },  // church
    { 0x26F2, 0x26F3, 2 },  // fountain, flag in hole
    { 0x26F5, 0x26F5, 2 },  // sailboat
    { 0x26FA, 0x26FA, 2 },  // tent
    { 0x26FD, 0x26FD, 2 },  // fuel pump
    { 0x2705, 0x2705, 2 },  // white heavy check mark
    { 0x270A, 0x270B, 2 },  // raised fist, raised hand
    { 0x2728, 0x2728, 2 },  // sparkles
    { 0x274C, 0x274C, 2 },  // cross mark
    { 0x274E, 0x274E, 2 },  // negative squared cross mark
    { 0x2753, 0x2755, 2 },  // question/exclamation ornament marks
    { 0x2757, 0x2757, 2 },  // heavy exclamation mark
    { 0x2795, 0x2797, 2 },  // heavy plus/minus/division signs
    { 0x27B0, 0x27B0, 2 },  // curly loop
    { 0x27BF, 0x27BF, 2 },  // double curly loop
    { 0x2B1B, 0x2B1C, 2 },  // black/white large squares
    { 0x2B50, 0x2B50, 2 },  // star
    { 0x2B55, 0x2B55, 2 },  // heavy large circle
    { 0x2E80, 0xA4CF, 2 },  // CJK radicals..Yi (existing EAW range)
    { 0xA960, 0xA97C, 2 },  // Hangul Jamo Extended-A
    { 0xAC00, 0xD7A3, 2 },  // Hangul syllables
    { 0xF900, 0xFAFF, 2 },  // CJK compatibility ideographs
    { 0xFE10, 0xFE19, 2 },  // vertical forms
    { 0xFE30, 0xFE6F, 2 },  // CJK compatibility forms
    { 0xFF01, 0xFF60, 2 },  // fullwidth forms
    { 0xFFE0, 0xFFE6, 2 },  // fullwidth signs
    { 0x1F000, 0x1F1E5, 2 }, // mahjong tiles.. (existing range, minus RI)
    { 0x1F200, 0x1F644, 2 }, // enclosed ideographic supplement..rolling eyes
    { 0x1F300, 0x1F6FF, 2 }, // cyclone..transport symbols
    { 0x1F7E0, 0x1F7EB, 2 }, // large colored circles
    { 0x1F7F0, 0x1F7F0, 2 }, // heavy equals sign
    { 0x1F900, 0x1F9FF, 2 }, // supplemental symbols and pictographs
    { 0x1FA70, 0x1FAFF, 2 }, // symbols and pictographs extended-A
    { 0x20000, 0x2FFFD, 2 }, // CJK extension B..F
    { 0x30000, 0x3FFFD, 2 }, // CJK extension G
};

static int oracleWidth(uint32_t cp)
{
    for (size_t i = 0; i < sizeof(kZeroWidth) / sizeof(kZeroWidth[0]); ++i)
        if (cp >= kZeroWidth[i].lo && cp <= kZeroWidth[i].hi)
            return 0;
    // Regional indicators: width 1 each (must win over the coarse wide
    // ranges below — the grid cannot represent flag pairs).
    if (cp >= 0x1F1E6 && cp <= 0x1F1FF)
        return 1;
    for (size_t i = 0; i < sizeof(kWide) / sizeof(kWide[0]); ++i)
        if (cp >= kWide[i].lo && cp <= kWide[i].hi)
            return 2;
    return 1;
}

// ---------------------------------------------------------------------------
// UTF-8 decode helper (for computing per-row oracle sums from the fixture).
// ---------------------------------------------------------------------------

static uint32_t utf8DecodeNext(const char*& s, const char* end)
{
    const unsigned char c0 = (unsigned char)*s++;
    if (c0 < 0x80)
        return c0;
    int n;
    uint32_t cp;
    if ((c0 & 0xE0) == 0xC0)      { n = 1; cp = c0 & 0x1F; }
    else if ((c0 & 0xF0) == 0xE0) { n = 2; cp = c0 & 0x0F; }
    else if ((c0 & 0xF8) == 0xF0) { n = 3; cp = c0 & 0x07; }
    else                          { return 0xFFFD; }
    while (n-- > 0 && s < end)
    {
        const unsigned char cc = (unsigned char)*s++;
        if ((cc & 0xC0) != 0x80)
            return 0xFFFD;
        cp = (cp << 6) | (cc & 0x3F);
    }
    return cp;
}

// VS16-eligible bases for the folding-aware oracle — mirrors
// imgui_draw.cpp's imtui_is_vs16_eligible (Emoji=Yes but
// Emoji_Presentation=No, emoji-data.txt 15.1, sub-0x1F000 subset). Kept
// minimal and conservative like the production list; the fixture only
// exercises U+26A0.
static bool oracleVs16Eligible(uint32_t cp)
{
    if (cp == 0x00A9 || cp == 0x00AE) return true;
    if (cp == 0x203C || cp == 0x2049) return true;
    if (cp == 0x2122 || cp == 0x2139) return true;
    if (cp >= 0x2194 && cp <= 0x2199) return true;
    if (cp >= 0x21A9 && cp <= 0x21AA) return true;
    if (cp == 0x2328 || cp == 0x23CF) return true;
    if (cp == 0x24C2) return true;
    if (cp >= 0x25AA && cp <= 0x25AB) return true;
    if (cp == 0x25B6 || cp == 0x25C0) return true;
    if (cp >= 0x25FB && cp <= 0x25FC) return true;
    if (cp >= 0x2600 && cp <= 0x2604) return true;
    if (cp == 0x260E) return true;
    if (cp == 0x2611 || cp == 0x2618) return true;
    if (cp == 0x261D) return true;
    if (cp == 0x2620) return true;
    if (cp >= 0x2622 && cp <= 0x2623) return true;
    if (cp == 0x2626 || cp == 0x262A) return true;
    if (cp >= 0x262E && cp <= 0x262F) return true;
    if (cp >= 0x2638 && cp <= 0x263A) return true;
    if (cp == 0x2640 || cp == 0x2642) return true;
    if (cp >= 0x265F && cp <= 0x2660) return true;
    if (cp == 0x2663) return true;
    if (cp >= 0x2665 && cp <= 0x2666) return true;
    if (cp == 0x2668) return true;
    if (cp == 0x267B || cp == 0x267E) return true;
    if (cp == 0x2692) return true;
    if (cp >= 0x2694 && cp <= 0x2697) return true;
    if (cp == 0x2699) return true;
    if (cp >= 0x269B && cp <= 0x269C) return true;
    if (cp == 0x26A0 || cp == 0x26A7) return true;
    if (cp >= 0x26B0 && cp <= 0x26B1) return true;
    if (cp == 0x26C8 || cp == 0x26CF) return true;
    if (cp == 0x26D1 || cp == 0x26D3) return true;
    if (cp == 0x26E9) return true;
    if (cp >= 0x26F0 && cp <= 0x26F1) return true;
    if (cp == 0x26F4) return true;
    if (cp >= 0x26F7 && cp <= 0x26F9) return true;
    if (cp == 0x2702) return true;
    if (cp >= 0x2708 && cp <= 0x2709) return true;
    if (cp >= 0x270C && cp <= 0x270D) return true;
    if (cp == 0x270F || cp == 0x2712) return true;
    if (cp == 0x2714 || cp == 0x2716) return true;
    if (cp == 0x271D || cp == 0x2721) return true;
    if (cp >= 0x2733 && cp <= 0x2734) return true;
    if (cp == 0x2744 || cp == 0x2747) return true;
    if (cp >= 0x2763 && cp <= 0x2764) return true;
    if (cp == 0x27A1) return true;
    if (cp >= 0x2934 && cp <= 0x2935) return true;
    if (cp >= 0x2B05 && cp <= 0x2B07) return true;
    if (cp == 0x3030 || cp == 0x303D) return true;
    if (cp == 0x3297 || cp == 0x3299) return true;
    return false;
}

// Oracle width sum of a whole row string (includes spaces: spaces emit no
// quad but advance the pen by 1, so they count for position/extent).
// Folding mode (g_folding): a VS16 (U+FE0F) directly following a
// VS16-eligible width-1 base promotes the pair from 1 to 2 cells, mirroring
// the production RenderText fold (imgui_draw.cpp imtui_is_vs16_eligible +
// the promotion guard `base width <= 1`). Combining marks and dropped
// format chars still contribute 0.
static int oracleRowSum(const char* row)
{
    int sum = 0;
    uint32_t prevCp = 0;
    int prevW = 0;
    const char* s = row;
    const char* end = row + strlen(row);
    while (s < end)
    {
        const uint32_t cp = utf8DecodeNext(s, end);
        const int w = oracleWidth(cp);
        if (g_folding && cp == 0xFE0F && prevW == 1 && oracleVs16Eligible(prevCp))
            sum += 1; // VS16 promotes the base from 1 to 2 cells
        else
            sum += w;
        prevCp = cp;
        prevW = w;
    }
    return sum;
}

// ---------------------------------------------------------------------------
// Grid probes
// ---------------------------------------------------------------------------

static bool isTextCell(const ImTui::TCell& c)
{
    return c.ch != 0 && c.ch != ' ';
}

// Leftmost text cell column in a grid row; -1 if none.
static int rowFirstTextCol(const ImTui::TScreen& s, int row)
{
    for (int x = 0; x < s.nx; ++x)
        if (isTextCell(s.data[row * s.nx + x]))
            return x;
    return -1;
}

// Rightmost text cell column in a grid row; -1 if none.
static int rowLastTextCol(const ImTui::TScreen& s, int row)
{
    for (int x = s.nx - 1; x >= 0; --x)
        if (isTextCell(s.data[row * s.nx + x]))
            return x;
    return -1;
}

// Rightmost rect cell column (ch == ' ', chwidth == 0, non-default bg)
// within [0, max_col]; -1 if none. Used to locate the scrollbar column.
static int rowLastRectCol(const ImTui::TScreen& s, int row, int max_col)
{
    for (int x = max_col; x >= 0; --x)
    {
        const ImTui::TCell& c = s.data[row * s.nx + x];
        if (c.ch == ' ' && c.chwidth == 0 && c.bg != 0)
            return x;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

// 12 rows; rows 2..7 are the six markdown-table rows from the bug report
// (shift pattern 0, 0, +1, +1, -1, +1). Width-0 codepoints are written as
// escapes so they are visible in source: U+0301 (combining acute), U+200D
// (ZWJ), U+FE0F (VS16).
static const char* const kRows[12] =
{
    "a\u2705b",                        // 0: probe row for assertion (b)
    "e\u0301\u200Db",                  // 1: combining mark + ZWJ (dropped)
    "| plain | a |",                   // 2: table row 0 (plain)
    "| plain | b |",                   // 3: table row 1 (plain)
    "| \u2705 | task1 |",              // 4: table row 2 (U+2705)
    "| \u2753 | ask2 |",               // 5: table row 3 (U+2753)
    "| \u26A0\uFE0F | warn3 |",        // 6: table row 4 (U+26A0 + VS16)
    "| \u2705 | done4 |",              // 7: table row 5 (U+2705)
    "filler eight",                    // 8..11: filler so content exceeds the
    "filler nine",                     // child height and a vertical scrollbar
    "filler ten",                      // appears (12 rows > 8-row child).
    "filler eleven",
};

static const int kRowCount = (int)(sizeof(kRows) / sizeof(kRows[0]));
static const int kFirstTableRow = 2;   // rows 2..7 are the six table rows
static const int kTableRowCount = 6;
static const int kChildWidth = 70;     // BeginChild size, mirroring tui.cpp
static const int kChildHeight = 8;
static const int kScreenW = 80;
static const int kScreenH = 24;

// ---------------------------------------------------------------------------
// Check framework
// ---------------------------------------------------------------------------

static int g_failures = 0;

static void check(bool ok, const char* what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++g_failures;
}

static void dumpGrid(const ImTui::TScreen& s)
{
    printf("Grid dump (rows 0..%d, cells shown as U+XXXX(chwidth[,ch2]) for"
           " text, '.' for rect cells; folding=%d):\n", kRowCount - 1,
           g_folding ? 1 : 0);
    for (int y = 0; y < kRowCount; ++y)
    {
        char line[512];
        int pos = 0;
        for (int x = 0; x < s.nx && pos < (int)sizeof(line) - 32; ++x)
        {
            const ImTui::TCell& c = s.data[y * s.nx + x];
            if (isTextCell(c))
            {
                if (c.ch2)
                    pos += snprintf(line + pos, sizeof(line) - pos,
                                    "U+%04X(%u,+U+%04X)@%d ", c.ch,
                                    (unsigned)c.chwidth, c.ch2, x);
                else
                    pos += snprintf(line + pos, sizeof(line) - pos,
                                    "U+%04X(%u)@%d ", c.ch, (unsigned)c.chwidth, x);
            }
            else if (c.ch == ' ' && c.chwidth == 0 && c.bg != 0)
                pos += snprintf(line + pos, sizeof(line) - pos, ".@%d ", x);
        }
        printf("  row %2d: %s\n", y, line);
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImTui_ImplText_Init();

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)kScreenW, (float)kScreenH);

    ImTui::TScreen screen;

    // Render several frames: the child's vertical scrollbar state is computed
    // from the previous frame's content size, so it only appears from frame 2
    // on; assertions run against the last frame's grid.
    for (int frame = 0; frame < 4; ++frame)
    {
        ImTui_ImplText_NewFrame();
        ImGui::NewFrame();

        // Root window, mirroring cpp_tui/tui.cpp renderFrame (1073-1084):
        // BeginChild must be nested inside an explicit Begin/End block —
        // the implicit fallback window is auto-positioned (centered), which
        // pushes the child off-screen and clips all content.
        ImGuiWindowFlags rootFlags = ImGuiWindowFlags_NoResize |
                                     ImGuiWindowFlags_NoTitleBar |
                                     ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_NoScrollbar |
                                     ImGuiWindowFlags_NoScrollWithMouse |
                                     ImGuiWindowFlags_NoBackground;
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2((float)kScreenW, (float)kScreenH),
                                 ImGuiCond_Always);
        ImGui::Begin("##TuiRoot", nullptr, rootFlags);

        ImGui::SetCursorPos(ImVec2(0, 0));
        const bool child_ok = ImGui::BeginChild("llm_output", ImVec2((float)kChildWidth,
                                               (float)kChildHeight),
                          false, ImGuiWindowFlags_HorizontalScrollbar);
        if (frame == 3)
            check(child_ok, "child window opens (not clipped)");
        for (int i = 0; i < kRowCount; ++i)
            ImGui::TextUnformatted(kRows[i]);
        ImGui::EndChild();

        ImGui::End();

        ImGui::Render();
        ImTui_ImplText_RenderDrawData(ImGui::GetDrawData(), &screen);
    }

    printf("Fixture oracle sums: ");
    for (int i = 0; i < kRowCount; ++i)
        printf("row%d=%d ", i, oracleRowSum(kRows[i]));
    printf("\n");
    dumpGrid(screen);

    // --- Layout sanity: row i must land on grid row i ---------------------
    // Each fixture row starts with an ASCII char; the child sits at (0,0)
    // (SetCursorPos above) and every TextUnformatted advances the line by
    // exactly FontSize (1.0), so grid row i holds kRows[i]. Rows beyond the
    // child height are clipped and must not be expected to render.
    for (int i = 0; i < kChildHeight && i < kRowCount; ++i)
    {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "layout: fixture row %d starts on grid row %d", i, i);
        const int first_col = rowFirstTextCol(screen, i);
        check(first_col >= 0 && screen.data[i * screen.nx + first_col].ch
              == (uint32_t)(unsigned char)kRows[i][0], msg);
    }

    // --- (b) probe row "a✅b": wide char lands +2, chwidth == 2 ------------
    {
        const int row = 0;
        const int a_col = rowFirstTextCol(screen, row);
        if (a_col < 0 || a_col + 3 >= screen.nx)
        {
            check(false, "(b) probe row not rendered (a_col out of range)");
            return 1;
        }
        const ImTui::TCell& ca = screen.data[row * screen.nx + a_col];
        const ImTui::TCell& ce = screen.data[row * screen.nx + a_col + 1];
        const ImTui::TCell& gap = screen.data[row * screen.nx + a_col + 2];
        const ImTui::TCell& cb = screen.data[row * screen.nx + a_col + 3];

        check(ca.ch == 'a', "(b) probe row starts with 'a'");
        check(ce.ch == 0x2705, "(b) U+2705 cell is directly after 'a'");
        check(ce.chwidth == 2, "(b) U+2705 cell reports chwidth == 2");
        check(!isTextCell(gap), "(b) second column of wide char is empty");
        check(cb.ch == 'b' && a_col + 3 == rowLastTextCol(screen, row),
              "(b) 'b' lands at 'a' column + 3 (fails pre-fix: +2)");
    }
    // --- (a) per-row extent == base_col + oracle_sum - 1 -------------------
    {
        const int base = rowFirstTextCol(screen, 0); // same pen start for all rows
        for (int t = 0; t < kTableRowCount; ++t)
        {
            const int i = kFirstTableRow + t;
            const int sum = oracleRowSum(kRows[i]);
            const int last = rowLastTextCol(screen, i);
            const int expected = base + sum - 1;
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "(a) table row %d '%s': extent %d == base %d + oracle %d - 1"
                     " (fails pre-fix: %s)", i, kRows[i], last, base, sum,
                     (i == 6) ? "VS16 counted" : "emoji counted narrow");
            check(last == expected, msg);
            char msg2[128];
            snprintf(msg2, sizeof(msg2),
                     "(a) table row %d starts at base column %d", i, base);
            check(rowFirstTextCol(screen, i) == base, msg2);
        }
    }

    // --- (c) no width-0 codepoint leaks into the grid ----------------------
    // ch must never hold a zero-width corpus codepoint (VS16/ZWJ/combining
    // mark); ch2 may hold only an allowed continuation (0 = none, a VS16 or
    // a combining mark — never ZWJ/format chars); every non-space text cell
    // has chwidth >= 1. Default mode (no folding): ch2 == 0 everywhere.
    {
        int leaks = 0;
        int ch2sets = 0;
        for (int y = 0; y < kRowCount; ++y)
            for (int x = 0; x < screen.nx; ++x)
            {
                const ImTui::TCell& c = screen.data[y * screen.nx + x];
                if (c.ch == 0xFE0F || c.ch == 0x200D || c.ch == 0x0301)
                    ++leaks;
                if (isTextCell(c) && c.chwidth == 0)
                    ++leaks;
                if (c.ch2 != 0)
                {
                    ++ch2sets;
                    // Allowed continuations: VS16 and the combining-mark
                    // blocks folded by the production rule. ZWJ (U+200D),
                    // ZWSP and other format chars must never be a ch2.
                    const bool ok2 = (c.ch2 == 0xFE0F) ||
                                     (c.ch2 >= 0x0300 && c.ch2 <= 0x036F) ||
                                     (c.ch2 >= 0x1AB0 && c.ch2 <= 0x1AFF) ||
                                     (c.ch2 >= 0x1DC0 && c.ch2 <= 0x1DFF) ||
                                     (c.ch2 >= 0x20D0 && c.ch2 <= 0x20FF) ||
                                     (c.ch2 >= 0xFE20 && c.ch2 <= 0xFE2F);
                    if (!ok2)
                        ++leaks;
                }
            }
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "(c) no zero-width leak: ch clean, ch2 only VS16/marks, "
                 "chwidth >= 1 (leaks=%d, ch2set=%d)", leaks, ch2sets);
        check(leaks == 0, msg);
        if (!g_folding)
        {
            char msg2[128];
            snprintf(msg2, sizeof(msg2),
                     "(c) default mode: nothing folds, all ch2 == 0 (ch2set=%d)",
                     ch2sets);
            check(ch2sets == 0, msg2);
        }
    }

    // --- (f) P3 Task 6 folding (only asserted in folding mode) -------------
    if (g_folding)
    {
        // Row 1 "e<U+0301><U+200D>b": the combining mark merges into 'e'
        // (ch2 = U+0301, width unchanged); ZWJ is dropped; 'b' follows
        // immediately.
        {
            const int row = 1;
            const int e_col = rowFirstTextCol(screen, row);
            const bool e_ok = e_col >= 0 &&
                              screen.data[row * screen.nx + e_col].ch == 'e' &&
                              screen.data[row * screen.nx + e_col].ch2 == 0x0301 &&
                              screen.data[row * screen.nx + e_col].chwidth == 1;
            check(e_ok, "(f) folding: 'e' cell merges U+0301 (ch2 set, width 1)");
            const bool b_ok = e_col >= 0 && e_col + 1 < screen.nx &&
                              screen.data[row * screen.nx + e_col + 1].ch == 'b' &&
                              e_col + 1 == rowLastTextCol(screen, row);
            check(b_ok, "(f) folding: 'b' lands directly after 'e' (mark adds no cell)");
        }
        // Row 6 "| <U+26A0><U+FE0F> | warn3 |": VS16 promotes the U+26A0
        // base to width 2 with ch2 = U+FE0F; no standalone VS16 cell exists
        // (covered by (c) too); the row extent follows the folding oracle.
        {
            const int row = 6;
            int w_col = -1;
            for (int x = 0; x < screen.nx; ++x)
                if (screen.data[row * screen.nx + x].ch == 0x26A0)
                    { w_col = x; break; }
            const bool w_ok = w_col > 0 &&
                              screen.data[row * screen.nx + w_col].ch2 == 0xFE0F &&
                              screen.data[row * screen.nx + w_col].chwidth == 2;
            check(w_ok, "(f) folding: U+26A0 cell promoted to width 2 with ch2 = U+FE0F");
            const bool gap_ok = w_col > 0 && w_col + 1 < screen.nx &&
                                !isTextCell(screen.data[row * screen.nx + w_col + 1]);
            check(gap_ok, "(f) folding: second column of the promoted pair is empty");
        }
    }
    else
    {
        // Default mode regression guard: the P0 narrow base must hold —
        // row 6's U+26A0 cell stays width 1 with no continuation.
        const int row = 6;
        int w_col = -1;
        for (int x = 0; x < screen.nx; ++x)
            if (screen.data[row * screen.nx + x].ch == 0x26A0)
                { w_col = x; break; }
        const bool w_ok = w_col > 0 &&
                          screen.data[row * screen.nx + w_col].ch2 == 0 &&
                          screen.data[row * screen.nx + w_col].chwidth == 1;
        check(w_ok, "(f) default mode: U+26A0 stays narrow width 1, no ch2");
    }

    // --- (d) scrollbar column constant across the six table rows -----------
    {
        int sb_cols[kTableRowCount];
        for (int t = 0; t < kTableRowCount; ++t)
            sb_cols[t] = rowLastRectCol(screen, kFirstTableRow + t, kChildWidth);
        bool constant = true;
        for (int t = 1; t < kTableRowCount; ++t)
            if (sb_cols[t] != sb_cols[0])
                constant = false;
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "(d) scrollbar column constant across table rows (%d %d %d %d %d %d)",
                 sb_cols[0], sb_cols[1], sb_cols[2],
                 sb_cols[3], sb_cols[4], sb_cols[5]);
        check(constant && sb_cols[0] > 0, msg);

        // (e) content never overwrites the scrollbar column (gated on (d)
        // having found a scrollbar column — if sb_cols[0] == -1, (d) already
        // failed and the vacuous scan would add nothing)
        if (sb_cols[0] > 0)
        {
            bool overwrite = false;
            for (int t = 0; t < kTableRowCount; ++t)
            {
                const int y = kFirstTableRow + t;
                for (int x = 0; x < screen.nx; ++x)
                    if (isTextCell(screen.data[y * screen.nx + x]) && x == sb_cols[0])
                        overwrite = true;
            }
            check(!overwrite, "(e) content cells never overwrite the scrollbar column");
        }
        else
        {
            printf("skip (e): no scrollbar column found (see (d))\n");
        }
    }

    printf("\n%s (%d failure%s)\n",
           g_failures == 0 ? "ALL CHECKS PASSED" : "CHECKS FAILED",
           g_failures, g_failures == 1 ? "" : "s");

    ImTui_ImplText_Shutdown();  // matches tuiShutdown; currently a no-op
    ImGui::DestroyContext();
    return g_failures == 0 ? 0 : 1;
}
