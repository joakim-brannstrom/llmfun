// Regression test for the VS16-fold row-rewrite corruption in the ncurses
// backend (llmfun imtui, P3 Task 6 follow-up fix).
//
// Background: with LLMFUN_IMTUI_EMOJI_PRESENTATION=1, RenderText folds a
// VS16 (U+FE0F) into its base cell (ch=0x26A0, ch2=0xFE0F, chwidth=2).
// ncurses' own width model counts the pair as ONE cell (wcwidth(U+26A0)=1,
// wcwidth(U+FE0F)=0) while the grid - and every VS16-clustering terminal
// (VTE/kitty/wezterm) - counts TWO. The first write of a row is still
// correct (the terminal applies its own model to the byte stream), but
// ncurses' virtual screen ends up 1 column short per pair, so when the row
// is REWRITTEN after a content change (typing, streaming, scrolling),
// ncurses' diff-based patches (absolute cursor moves + single-cell writes)
// land 1 column LEFT of the real position: the row shifts left, new
// characters overwrite their predecessors and stale characters linger
// (the reported " foobar" -> " ffoobar" bug).
//
// Fix under test: ImTui_ImplNcurses_DrawScreen forces a full-line redraw
// (wredrawln) for every written row whose new or previous grid contains a
// folded continuation, so ncurses re-emits the whole line from column 0
// instead of patching it.
//
// THIS BINARY ONLY PRODUCES THE RAW NCURSES STREAM on stdout (it must run
// on a PTY). The Python driver test_ncurses_fold_redraw.py spawns it on a
// pty, replays the stream against a VS16-clustering terminal model
// (pair = 2 cells) and asserts the final row content. That is the only
// layer where the bug is observable: the TScreen grid is internally
// consistent pre-fix, and tmux counts the pair as 1 cell (like ncurses),
// so neither a headless grid test nor a tmux capture can see it.
//
// Usage: test_ncurses_fold_redraw [3|4]   (default 3 frames)
//   - 3 frames: pair present the whole time, row rewritten twice -> the
//     pre-fix diff patches land 1 left and corrupt the row tail;
//   - 4 frames: adds the fold-removal transition (pair -> plain ⚠), which
//     must also force a full re-sync because the PREVIOUS grid carried
//     the fold.
#include "imtui/imtui.h"
#include "imtui/imtui-impl-ncurses.h"

#include <clocale>
#include <cstdlib>
#include <unistd.h>

static void setCell(ImTui::TScreen* screen, int y, int x,
                    ImTui::TColor fg, ImTui::TColor bg, uint32_t ch,
                    uint8_t chwidth, uint32_t ch2 = 0) {
    ImTui::TCell& cell = screen->data[y * screen->nx + x];
    cell.fg = fg;
    cell.bg = bg;
    cell.ch = ch;
    cell.chwidth = chwidth;
    cell.ch2 = ch2;
}

static void setSpace(ImTui::TScreen* screen, int y, int x,
                     ImTui::TColor fg, ImTui::TColor bg) {
    setCell(screen, y, x, fg, bg, 0, 0);
}

int main(int argc, char** argv) {
    // Same as the real TUI (tui.cpp): without a UTF-8 locale ncurses
    // replaces non-ASCII chars (the VS16 pair) with spaces.
    setlocale(LC_ALL, "");
    const int frames = (argc > 1) ? atoi(argv[1]) : 3;

    ImGui::CreateContext();
    ImTui::TScreen* screen = ImTui_ImplNcurses_Init(false, 60.0, 60.0);
    // NOTE: 80x24 is coupled to the PTY driver (test_ncurses_fold_redraw.py
    // sets TIOCSWINSZ to 80x24 and replays against an 80x24 grid); change
    // both together if either side ever needs a different geometry.
    screen->resize(80, 24);
    screen->clear();

    // ANSI palette values like the real text backend produces
    // (rgbToAnsi256): distinct fg per logical color run so the emitter
    // splits the row into runs exactly like a markdown table row does.
    const ImTui::TColor kDefault = 16;  // ~black (bg)
    const ImTui::TColor kText = 7;      // white (cell text)
    const ImTui::TColor kPipe = 3;      // yellow (table separators)
    const ImTui::TColor kFrame = 2;     // green (trailing padding run)

    // Row 0: markdown table row "| ⚠️ | foobar |" with color runs:
    //   [pipe " |"] [text " ⚠️"] [pipe " | "] [text "foobar"] [pipe " |"]
    //   [frame padding ...]
    {
        int x = 0;
        setSpace(screen, 0, x++, kPipe, kDefault);
        setCell(screen, 0, x++, kPipe, kDefault, '|', 1);
        setCell(screen, 0, x++, kText, kDefault, ' ', 1);
        setCell(screen, 0, x++, kText, kDefault, 0x26A0, 2, 0xFE0F);
        setSpace(screen, 0, x++, kText, kDefault);  // placeholder: skipped by the emitter
        setCell(screen, 0, x++, kPipe, kDefault, ' ', 1);
        setCell(screen, 0, x++, kPipe, kDefault, '|', 1);
        setCell(screen, 0, x++, kPipe, kDefault, ' ', 1);
        const char* txt = "foobar";
        for (const char* p = txt; *p; ++p)
            setCell(screen, 0, x++, kText, kDefault, (uint32_t)*p, 1);
        setCell(screen, 0, x++, kPipe, kDefault, ' ', 1);
        setCell(screen, 0, x++, kPipe, kDefault, '|', 1);
        while (x < 60)
            setSpace(screen, 0, x++, kFrame, kDefault);
    }

    // Row 1: plain ASCII control row "  foobar" + frame padding (no fold).
    // Must survive all frames untouched: it pins the emitter's per-row
    // diff to the same rewrite discipline as the real TUI.
    {
        int x = 2;
        const char* txt = "foobar";
        for (const char* p = txt; *p; ++p)
            setCell(screen, 1, x++, kText, kDefault, (uint32_t)*p, 1);
        while (x < 60)
            setSpace(screen, 1, x++, kFrame, kDefault);
    }

    // Frame 1: initial full render.
    ImTui_ImplNcurses_DrawScreen(true);
    usleep(200 * 1000);

    // Frame 2: content change -> fold row is rewritten (streaming/typing
    // simulation). Pre-fix this emits an absolute patch that lands 1 left.
    setCell(screen, 0, 14, kText, kDefault, 'Z', 1);
    ImTui_ImplNcurses_DrawScreen(true);
    usleep(200 * 1000);

    // Frame 3: second change.
    setCell(screen, 0, 15, kText, kDefault, '!', 1);
    ImTui_ImplNcurses_DrawScreen(true);
    usleep(200 * 1000);

    // Frame 4 (optional): fold removal transition (⚠️ -> ⚠). The previous
    // grid still carried the fold, so the row must be fully re-synced as
    // well (ncurses' virtual line is the divergent 1-cell model).
    if (frames >= 4) {
        setCell(screen, 0, 3, kText, kDefault, 0x26A0, 1, 0);
        ImTui_ImplNcurses_DrawScreen(true);
        usleep(200 * 1000);
    }

    // Settle frame: flushes the previous frame's writes to the terminal.
    // DrawScreen only refreshes at its START (the vsync wait at the end
    // returns immediately while the timing budget is stale), so without
    // one more call the last frame's output would never reach the PTY.
    ImTui_ImplNcurses_DrawScreen(true);
    usleep(200 * 1000);

    ImTui_ImplNcurses_Shutdown();
    return 0;
}
