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

    bool operator==(const TCell& o) const {
        return ch == o.ch && fg == o.fg && bg == o.bg && chwidth == o.chwidth;
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
