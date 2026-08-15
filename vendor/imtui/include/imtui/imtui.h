/*! \file imtui.h
 *  \brief Enter description here.
 */

#pragma once

// simply expose the existing Dear ImGui API
#include "imgui/imgui.h"

#include "imtui/imtui-impl-text.h"

#include <cstring>
#include <cstdint>

namespace ImTui {

using TChar = unsigned char;
using TColor = unsigned char;

// single screen cell
struct TCell
{
    TColor fg;
    TColor bg;
    uint32_t ch;
    uint8_t chwidth;
    // LLMFUN PATCH (P3, Task 6): zero-width continuation codepoint attached
    // to this cell by the RenderText folding rule (imgui_draw.cpp) — a VS16
    // (U+FE0F) selecting emoji presentation for the base, or a combining
    // mark (U+0300-U+036F etc.) merged into the base glyph. Examples:
    //   U+26A0 + U+FE0F -> ch = U+26A0, ch2 = U+FE0F, chwidth = 2
    //   'e'  + U+0301  -> ch = 'e',     ch2 = U+0301,  chwidth = 1
    // 0 = none. Folding is opt-in (env LLMFUN_IMTUI_EMOJI_PRESENTATION=1,
    // terminals that cluster VS16); otherwise ch2 stays 0 everywhere and
    // the P0 behavior (Task 2: drop the continuation, narrow base) applies.
    // Rect cells (scrollbar/window bg, drawTriangle in imtui-impl-text.cpp)
    // never set it. The ncurses backend emits ch followed by ch2 as one
    // addwstr run before the cursor compensation, so the terminal clusters
    // the pair. operator== below MUST include ch2 — the ncurses per-row
    // diff (imtui-impl-ncurses.cpp) depends on it, and TCell must stay POD
    // for the memcpy diff paths.
    uint32_t ch2 = 0;

    bool operator==(const TCell& o) const {
        return ch == o.ch && ch2 == o.ch2 && fg == o.fg && bg == o.bg && chwidth == o.chwidth;
    }

    bool operator!=(const TCell& o) const {
        return !(*this == o);
    }
};

struct TScreen {
    int nx = 0;
    int ny = 0;

    int nmax = 0;

    TCell * data = nullptr;

    ~TScreen() {
        if (data) delete [] data;
    }

    inline int size() const { return nx*ny; }

    inline void clear() {
        if (data) {
            memset(data, 0, nx*ny*sizeof(TCell));
        }
    }

    inline void resize(int pnx, int pny) {
        nx = pnx;
        ny = pny;
        if (nx*ny <= nmax) return;

        if (data) delete [] data;

        nmax = nx*ny;
        data = new TCell[nmax];
    }
};

}
