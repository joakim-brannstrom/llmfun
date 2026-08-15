#define IMTUI
#define IMGUI_USE_WCHAR32
#include "../third-party/imgui/imgui/imgui_draw.cpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct WidthCase
{
    unsigned int cp;
    int expected;
    bool emojiClass;   // expected becomes 1 when LLMFUN_IMTUI_EMOJI_WIDTH=1
    const char* name;
};

static const WidthCase kCases[] =
{
    { 0x2705, 2, true,  "U+2705  (check mark)" },
    { 0x2753, 2, true,  "U+2753  (question mark ornament)" },
    { 0x26A0, 1, false, "U+26A0  (warning sign, narrow without VS16)" },
    { 0x1F7E0, 2, true, "U+1F7E0 (large orange circle)" },
    { 0x1FAD0, 2, true, "U+1FAD0 (blueberries)" },
    { 0x1FB00, 1, false, "U+1FB00 (block sextant-1, not emoji-presentation)" },
    { 0x1F600, 2, true, "U+1F600 (grinning face)" },
    { 0x231A, 2, true,  "U+231A  (watch)" },
    { 0x4E2D, 2, false, "U+4E2D  (CJK ideograph; EAW wide, not the emoji class)" },
    { 0xFE0F, 0, false, "U+FE0F  (VS16 variation selector)" },
    { 0xFE0E, 0, false, "U+FE0E  (VS15 variation selector)" },
    { 0x200D, 0, false, "U+200D  (ZWJ zero width joiner)" },
    { 0x200B, 0, false, "U+200B  (ZWSP zero width space)" },
    { 0x0301, 0, false, "U+0301  (combining acute accent)" },
    { 0x0061, 1, false, "U+0061  (latin 'a')" },
    { 0x007F, 0, false, "U+007F  (DEL control)" },
    { 0xFF21, 2, false, "U+FF21  (fullwidth latin capital A; EAW, not the emoji class)" },
    { 0x2B1B, 2, true,  "U+2B1B  (black large square)" },
    { 0x2B50, 2, true,  "U+2B50  (star)" },
    { 0x2B55, 2, true,  "U+2B55  (heavy large circle)" },
    { 0x2B05, 1, false, "U+2B05  (leftwards arrow, text-presentation default)" },
    { 0x1FAFF, 2, true, "U+1FAFF (boundary: last of 1FA70-1FAFF)" },
    { 0x1F1E6, 1, false, "U+1F1E6 (regional indicator A, flag pair)" },
    { 0x1F6FF, 2, true, "U+1F6FF (boundary: last of 1F300-1F6FF)" },
};

// P3 (Task 6) helper predicates exercised directly (TU inclusion makes the
// static functions visible here).
struct FlagCase
{
    unsigned int cp;
    bool (*fn)(unsigned int);
    bool expected;
    const char* name;
};

static const FlagCase kFlagCases[] =
{
    { 0x26A0, imtui_is_vs16_eligible, true,  "imtui_is_vs16_eligible(U+26A0) == true (text-default emoji)" },
    { 0x2764, imtui_is_vs16_eligible, true,  "imtui_is_vs16_eligible(U+2764) == true (red heart)" },
    { 0x2705, imtui_is_vs16_eligible, false, "imtui_is_vs16_eligible(U+2705) == false (already emoji-presentation)" },
    { 0x231A, imtui_is_vs16_eligible, false, "imtui_is_vs16_eligible(U+231A) == false (already emoji-presentation)" },
    { 0x0061, imtui_is_vs16_eligible, false, "imtui_is_vs16_eligible('a') == false" },
    { 0x0301, imtui_is_combining_mark, true,  "imtui_is_combining_mark(U+0301) == true" },
    { 0x20D0, imtui_is_combining_mark, true,  "imtui_is_combining_mark(U+20D0) == true" },
    { 0xFE20, imtui_is_combining_mark, true,  "imtui_is_combining_mark(U+FE20) == true" },
    { 0xFE0F, imtui_is_combining_mark, false, "imtui_is_combining_mark(U+FE0F) == false (variation selector, not a mark)" },
    { 0x200D, imtui_is_combining_mark, false, "imtui_is_combining_mark(U+200D) == false (ZWJ)" },
    { 0x0065, imtui_is_combining_mark, false, "imtui_is_combining_mark('e') == false" },
    { 0x2705, imtui_is_emoji_presentation, true,  "imtui_is_emoji_presentation(U+2705) == true" },
    { 0x26A0, imtui_is_emoji_presentation, false, "imtui_is_emoji_presentation(U+26A0) == false" },
    { 0x1F600, imtui_is_emoji_presentation, true,  "imtui_is_emoji_presentation(U+1F600) == true" },
};

int main()
{
    // LLMFUN_IMTUI_EMOJI_WIDTH=1 forces the Emoji_Presentation=Yes class to
    // width 1 (narrow-emoji terminal mitigation). build_test.py runs this
    // binary twice: default (emoji class = 2) and with the env var set.
    const char * envW = getenv("LLMFUN_IMTUI_EMOJI_WIDTH");
    const bool emojiWidthOne = (envW != NULL && strcmp(envW, "1") == 0);

    int failures = 0;
    const int total = (int)(sizeof(kCases) / sizeof(kCases[0]));

    for (int i = 0; i < total; ++i)
    {
        const int expected = (emojiWidthOne && kCases[i].emojiClass) ? 1 : kCases[i].expected;
        const int w = imtui_wcwidth(kCases[i].cp);
        const int wf = (int)ImFontIMTuiCellWidth(kCases[i].cp);
        const bool ok = (w == expected) && (wf == expected);
        printf("%s %s: imtui_wcwidth=%d ImFontIMTuiCellWidth=%d expected=%d%s\n",
               ok ? "ok  " : "FAIL", kCases[i].name, w, wf, expected,
               emojiWidthOne ? " (LLMFUN_IMTUI_EMOJI_WIDTH=1)" : "");
        if (!ok)
            failures++;
    }

    const int totalFlags = (int)(sizeof(kFlagCases) / sizeof(kFlagCases[0]));
    for (int i = 0; i < totalFlags; ++i)
    {
        const bool ok = (kFlagCases[i].fn(kFlagCases[i].cp) == kFlagCases[i].expected);
        printf("%s %s\n", ok ? "ok  " : "FAIL", kFlagCases[i].name);
        if (!ok)
            failures++;
    }

    if (failures > 0)
    {
        printf("%d/%d width and helper checks FAILED.\n", failures, total + totalFlags);
        return 1;
    }
    printf("All %d width and helper checks passed.\n", total + totalFlags);
    return 0;
}
