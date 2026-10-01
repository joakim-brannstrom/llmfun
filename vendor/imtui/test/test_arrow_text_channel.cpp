// test_arrow_text_channel.cpp
//
// Headless regression test for the arrow-via-text-channel design: Dear
// ImGui's collapsing-header markers (ImGui::RenderArrow) are emitted as
// TEXT-ENCODED quads (the ImFont::RenderText vertex-color contract: vtx[0]
// = fg color, vtx[1] = codepoint, vtx[2] = cell width, vtx[3] = continuation)
// and decoded by the imtui text backend into real arrow glyphs, instead of
// being rasterized as filled triangles whose 1-cell shape the backend used
// to GUESS at (the removed drawTriangle glyph detection, which also ate the
// InputText caret and manufactured spurious glyphs at scrollbar edges).
//
// This TU renders a CollapsingHeader + InputText UI through the REAL vendored
// imgui + imtui text backend (linked like llmfun_tui: imgui-for-imtui is
// compiled with IMTUI + IMGUI_USE_WCHAR32 by the build; this TU itself defines
// neither) into an ImTui::TScreen grid and asserts, on the last frame's grid:
//
//   (a) each CollapsingHeader row shows exactly ONE arrow glyph with the
//       expected direction ('>' collapsed, 'v' open), the arrow cell keeps
//       the header band's bg (a block-painted arrow would carry the text
//       color as bg instead), and the arrow cell's fg equals the label's
//       text fg (both come from the same pushed ImGuiCol_Text);
//   (b) grid-wide glyph census: exactly 3 cells in the whole 80x24 grid
//       carry an arrow character ('<','>','^','v') — the 3 header arrows.
//       Fails if the InputText caret is misdecoded as a '>' glyph (the
//       reported regression) or if scrollbar/edge triangles manufacture
//       spurious glyphs;
//   (c) the focused InputText caret renders as a BLOCK: the cell right
//       after "hello" has ch == ' ' and a bg distinct from the field bg.
//
// Setup mirrors cpp_tui/tui.cpp (ImTui_ImplText_Init style defaults) and the
// fixture harness of test_utf8_grid.cpp: CreateContext; ImTui_ImplText_Init();
// io.DisplaySize = (80,24); per frame: ImTui_ImplText_NewFrame();
// ImGui::NewFrame(); root window at (0,0); child "llm_output" (70,19) with
// HorizontalScrollbar holding 22 header rows (vertical + horizontal
// scrollbars appear, recreating the geometry that used to produce the
// spurious scrollbar-edge glyphs); focused InputText below the child.
// Six frames are rendered; assertions run on the last frame's grid.
//
// Exit code 0 only if every check passes. Headless: no ncurses, no PTY.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "imtui/imtui.h"

static int g_fail = 0;

static void check(bool ok, const char * msg)
{
    printf("%s: %s\n", ok ? "ok  " : "FAIL", msg);
    if (!ok)
        ++g_fail;
}

static const int kScreenW    = 80;
static const int kScreenH    = 24;
static const int kChildW     = 70;
static const int kChildH     = 19;
static const int kFillerRows = 22;  // > kChildH -> vertical scrollbar appears
static const int kCaretRow   = 20;  // InputText below the child
static const int kFrames     = 6;

static bool isArrowChar(wchar_t c)
{
    return c == L'<' || c == L'>' || c == L'^' || c == L'v';
}

int main()
{
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImTui_ImplText_Init();

    ImGuiIO & io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)kScreenW, (float)kScreenH);

    ImTui::TScreen screen;

    char buf[16];
    snprintf(buf, sizeof(buf), "hello");

    std::string wideLabel = "Row wide " + std::string(100, 'x');

    for (int frame = 0; frame < kFrames; ++frame)
    {
        ImTui_ImplText_NewFrame();
        ImGui::NewFrame();

        ImGuiWindowFlags rootFlags = ImGuiWindowFlags_NoResize |
                                     ImGuiWindowFlags_NoTitleBar |
                                     ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_NoScrollbar |
                                     ImGuiWindowFlags_NoScrollWithMouse |
                                     ImGuiWindowFlags_NoBackground;
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2((float)kScreenW, (float)kScreenH), ImGuiCond_Always);
        ImGui::Begin("##TuiRoot", nullptr, rootFlags);

        ImGui::SetCursorPos(ImVec2(0, 0));
        ImGui::BeginChild("llm_output", ImVec2((float)kChildW, (float)kChildH), false,
                          ImGuiWindowFlags_HorizontalScrollbar);

        // Row 0: default-styled collapsed header -> '>' arrow.
        ImGui::CollapsingHeader("Assistant 9");

        // Row 1: blue open header -> 'v' arrow, band bg preserved.
        ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.16f, 0.39f, 0.64f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.16f, 0.39f, 0.64f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0.16f, 0.39f, 0.64f, 1.0f));
        ImGui::CollapsingHeader("Notes", ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopStyleColor(3);

        // Row 2: green collapsed header (the degenerate-1-cell-triangle
        // case) -> '>' arrow via the text channel like any other.
        ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.20f, 0.55f, 0.30f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.20f, 0.55f, 0.30f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0.20f, 0.55f, 0.30f, 1.0f));
        ImGui::CollapsingHeader("Files");
        ImGui::PopStyleColor(3);

        // Wide row + filler rows overflow the child -> both scrollbars.
        ImGui::TextUnformatted(wideLabel.c_str());
        for (int i = 4; i <= kFillerRows; ++i)
        {
            char lbl[32];
            snprintf(lbl, sizeof(lbl), "Row %d", i);
            ImGui::CollapsingHeader(lbl);
        }
        ImGui::EndChild();

        // Focused input field below the child: the caret must render as a
        // block, not as a guessed arrow glyph.
        ImGui::SetCursorPosY((float)kCaretRow);
        ImGui::SetKeyboardFocusHere();
        ImGui::InputText("##input", buf, sizeof(buf));

        ImGui::End();

        ImGui::Render();
        ImTui_ImplText_RenderDrawData(ImGui::GetDrawData(), &screen);
    }

    // Debug: dump the grid. Printable non-space chars print as themselves;
    // every other cell prints as the LOW HEX DIGIT of its bg (empty cells
    // have bg 235 -> 'B', so space-ch cells with a different bg are visible).
    for (int y = 0; y < screen.ny; ++y)
    {
        printf("r%02d|", y);
        for (int x = 0; x < screen.nx; ++x)
        {
            const auto & c = screen.data[y*screen.nx + x];
            if (c.ch && c.ch != L' ')
                printf("%lc", c.ch);
            else
                printf("%X", (unsigned)(c.bg & 0xF));
        }
        printf("|\n");
    }

    // --- (a) per-header arrow glyph checks ---------------------------------
    // The 3 special headers sit on grid rows 0, 1, 2 (child at (0,0), the
    // imtui style sets WindowPadding.y = 0, FontSize = 1).
    const wchar_t expectedDir[3] = { L'>', L'v', L'>' };
    const char * headerName[3]   = { "grey collapsed", "blue open", "green collapsed" };
    int arrowCol[3] = { -1, -1, -1 };

    for (int r = 0; r < 3; ++r)
    {
        int n = 0;
        for (int x = 0; x < screen.nx; ++x)
            if (screen.data[r*screen.nx + x].ch == expectedDir[r])
            {
                ++n;
                arrowCol[r] = x;
            }

        char msg[128];
        snprintf(msg, sizeof(msg), "%s header row shows exactly one '%lc' arrow glyph",
                 headerName[r], expectedDir[r]);
        check(n == 1, msg);
        if (n != 1)
            continue;

        // Band bg preserved: the arrow cell's bg equals the band bg a few
        // cells to the right (inside the header band; text cells keep the
        // band bg). A block-painted arrow would carry the TEXT color as bg.
        const int arrowBg  = screen.data[r*screen.nx + arrowCol[r]].bg;
        const int bandBg   = screen.data[r*screen.nx + arrowCol[r] + 6].bg;
        snprintf(msg, sizeof(msg), "%s header: arrow cell keeps the band bg (%d == %d)",
                 headerName[r], arrowBg, bandBg);
        check(arrowBg == bandBg, msg);

        // Arrow fg == label fg: both decode from the same pushed text color.
        int labelCol = -1;
        for (int x = 0; x < screen.nx; ++x)
            if (screen.data[r*screen.nx + x].ch != L' ')
            {
                labelCol = screen.data[r*screen.nx + x].fg;
                break;
            }
        snprintf(msg, sizeof(msg), "%s header: arrow fg equals label fg (%d == %d)",
                 headerName[r], screen.data[r*screen.nx + arrowCol[r]].fg, labelCol);
        check(labelCol >= 0 && screen.data[r*screen.nx + arrowCol[r]].fg == labelCol, msg);
    }

    // --- (b) grid-wide arrow census ----------------------------------------
    // Every visible header row inside the child carries exactly one arrow:
    // the 3 styled headers (rows 0-2) plus the collapsed filler headers
    // (rows 4..kChildH-2; row kChildH-1 is the horizontal scrollbar and row 3
    // is the wide text row, neither of which has an arrow) -> kChildH - 2
    // arrows. Catches the InputText caret being misdecoded as '>', and any
    // spurious glyph manufactured from scrollbar/edge triangles.
    int census = 0;
    for (int y = 0; y < screen.ny; ++y)
        for (int x = 0; x < screen.nx; ++x)
            if (isArrowChar(screen.data[y*screen.nx + x].ch))
                ++census;
    char msg[96];
    snprintf(msg, sizeof(msg), "grid-wide arrow census equals the %d visible header arrows (found %d)",
             kChildH - 2, census);
    check(census == kChildH - 2, msg);

    // --- (c) the InputText caret renders as a block -------------------------
    // Find the 'h' of "hello" on the caret row; the caret sits right after
    // the 5-character text. It must be an empty cell with a bg distinct from
    // the field bg (a painted block), not a glyph (the census above already
    // fails if it were '>').
    int hCol = -1;
    for (int x = 0; x < screen.nx; ++x)
        if (screen.data[kCaretRow*screen.nx + x].ch == L'h')
        {
            hCol = x;
            break;
        }
    check(hCol >= 0, "input text 'hello' is rendered on the caret row");
    if (hCol >= 0)
    {
        const int caretBg  = screen.data[kCaretRow*screen.nx + (hCol + 5)].bg;
        const int fieldBg  = screen.data[kCaretRow*screen.nx + (hCol + 7)].bg;
        char msg[128];
        snprintf(msg, sizeof(msg), "caret cell after \"hello\" is a block (bg %d != field bg %d, ch ' ')",
                 caretBg, fieldBg);
        check(screen.data[kCaretRow*screen.nx + (hCol + 5)].ch == L' ' && caretBg != fieldBg, msg);
    }

    printf(g_fail == 0 ? "ALL CHECKS PASSED\n" : "%d CHECK(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
