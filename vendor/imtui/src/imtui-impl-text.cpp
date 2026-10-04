/*! \file imtui-impl-text.cpp
 *  \brief Enter description here.
 */

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "imtui/imtui.h"
#include "imtui/imtui-impl-text.h"
#include <cstdio>
#include <cstdlib>
#include <cwchar>

#include <cmath>
#include <algorithm>
#include <vector>

#define ABS(x) ((x >= 0) ? x : -x)

// LLMFUN PATCH: per-row edge evaluation. The original incremental DDA
// stepped x only when the accumulated error counter k crossed m, but for
// near-vertical edges (|dy| >> |dx|) it advanced x once per |dy| rows, so
// depending on the phase the edge collapsed onto a single column: a
// 1-cell-wide vertical rect (a child-window scrollbar track/grab) then had a
// zero-width span on every row except the rows its horizontal edges touched,
// so the middle of the scrollbar was never painted even though its geometry
// was a full cell wide. Evaluate the crossing x for each row instead (edges
// are at most ydelta ≈ screen-height steps long).
void ScanLine(float x1, int y1, float x2, int y2, int ymax, std::vector<float> & xrange) {
    if (y1 == y2) {
        // Horizontal edge: contributes to its single row.
        if (y1 >= 0 && y1 < ymax) {
            if (x1 < xrange[2*y1+0]) xrange[2*y1+0] = x1;
            if (x1 > xrange[2*y1+1]) xrange[2*y1+1] = x1;
            if (x2 < xrange[2*y1+0]) xrange[2*y1+0] = x2;
            if (x2 > xrange[2*y1+1]) xrange[2*y1+1] = x2;
        }
        return;
    }

    const int ylo = std::min(y1, y2);
    const int yhi = std::max(y1, y2);
    for (int y = ylo; y <= yhi; ++y) {
        if (y < 0 || y >= ymax)
            continue;
        const float t = static_cast<float>(y - y1) / static_cast<float>(y2 - y1);
        const float x = x1 + (x2 - x1) * t;
        if (x < xrange[2*y+0]) xrange[2*y+0] = x;
        if (x > xrange[2*y+1]) xrange[2*y+1] = x;
    }
}

void drawTriangle(ImVec2 p0, ImVec2 p1, ImVec2 p2, unsigned char col, ImTui::TScreen * screen) {
    // LLMFUN PATCH: rasterize on the integer cell grid and count rows with the
    // half-open interval [ymin, ymax). Previously (a) fractional vertex ys
    // (e.g. the InputText caret/selection rects) set xrange at non-integer
    // indices via xrange[2*y] with fractional y, and (b) ydelta =
    // ymax-ymin+1 painted one extra row below widgets whose bottom edge lands
    // exactly on a cell boundary: a 1-unit-tall rect covering row r painted
    // rows r and r+1, so the InputText caret, the InputText field frame and
    // CollapsingHeader were rendered 2 terminal rows tall instead of 1.
    p0.y = std::floor(p0.y);
    p1.y = std::floor(p1.y);
    p2.y = std::floor(p2.y);

    int ymin = std::min(std::min(std::min((float) screen->size(), p0.y), p1.y), p2.y);
    float ymax = std::max(std::max(std::max(0.0f, p0.y), p1.y), p2.y);

    // LLMFUN PATCH: ceil the fractional ymax so the bottom row of a widget
    // whose bottom edge lands inside a cell (e.g. the scrollbar track's
    // bottom edge at y = 19.5 covering the top half of cell 19) is painted.
    // Truncation dropped that row: ydelta = (int)19.5 - 1 = 18 painted rows
    // 1..18 instead of 1..19.
    int ydelta = (int) std::ceil(ymax) - ymin;
    // LLMFUN PATCH: a triangle whose three vertices share one y after
    // flooring (e.g. the horizontal scrollbar grab / edge lines, which have
    // zero height in the ImGui coordinate space) got ydelta == 0:
    // ScanLine stored no spans and the paint loop painted no rows, so every
    // degenerate-height rect rendered invisibly in the text backend while
    // the vertical scrollbar worked. Paint one row covering the triangle
    // full x-extent.
    if (ydelta == 0) ydelta = 1;

    // LLMFUN PATCH: per-triangle span buffer. Each triangle rasterizes into
    // its own xrange sized [0, ydelta) and paints only its own spans, so
    // overlapping triangles keep painter's order (the last triangle to touch
    // a cell wins). A frame-wide static that accumulated all triangles'
    // spans made every triangle paint the union of all previous triangles'
    // extents, so e.g. the scrollbar track (rows 19-20) was repainted by
    // the later full-width window-background triangles with the window
    // color and vanished.
    std::vector<float> xrange(2*ydelta, 0.0f);
    for (int y = 0; y < ydelta; y++) {
        xrange[2*y+0] = 999999;
        xrange[2*y+1] = -999999;
    }

    ScanLine(p0.x, p0.y - ymin, p1.x, p1.y - ymin, ydelta, xrange);
    ScanLine(p1.x, p1.y - ymin, p2.x, p2.y - ymin, ydelta, xrange);
    ScanLine(p2.x, p2.y - ymin, p0.x, p0.y - ymin, ydelta, xrange);

    for (int y = 0; y < ydelta; y++) {
        if (xrange[2*y+1] >= xrange[2*y+0]) {
            // nearbyint(xmax)) on the float per-row span, with banker's
            // rounding (ties to even) for fractional edges. ScanLine now
            // stores per-row [xmin, xmax] as floats: a 0.5-unit-wide
            // scrollbar track at x [78.5, 79.0) maps to cells [78, 79) =
            // cell 78 only, and its degenerate right-edge line [79.0, 79.0]
            // maps to zero cells (invisible). Ties (x.5) round to even, so
            // the 6-px scrollbar grab [31.5, 37.5) stays 6 cells (32..37)
            // and the horizontal scrollbar [30.0, 78.5) stays 48 cells
            // (30..77).
            int xs = (int) std::nearbyint(xrange[2*y+0]);
            int xe = (int) std::nearbyint(xrange[2*y+1]);
            int len = xe - xs;
            int x = xs;

            while (len--) {
                if (x >= 0 && x < screen->nx && y + ymin >= 0 && y + ymin < screen->ny) {
                    auto & cell = screen->data[(y + ymin)*screen->nx + x];
                    cell.ch = ' ';
                    cell.bg = col;
                }
                ++x;
            }
        }
    }
}

inline ImTui::TColor rgbToAnsi256(ImU32 col, bool doAlpha) {
    ImTui::TColor r = col & 0x000000FF;
    ImTui::TColor g = (col & 0x0000FF00) >> 8;
    ImTui::TColor b = (col & 0x00FF0000) >> 16;

    if (r == g && g == b) {
        if (doAlpha) {
            ImTui::TColor a = (col & 0xFF000000) >> 24;
            r = (float(r)*a)/255.0f;
        }
        if (r < 8) {
            return 16;
        }

        if (r > 248) {
            return 231;
        }

        return std::round((float(r - 8) / 247) * 24) + 232;
    }

    if (doAlpha) {
        ImTui::TColor a = (col & 0xFF000000) >> 24;
        float scale = float(a)/255.0f;
        r = std::round(r*scale);
        g = std::round(g*scale);
        b = std::round(b*scale);
    }

    ImTui::TColor res = 16
        + (36 * std::round((float(r) / 255.0f) * 5.0f))
        + (6 * std::round((float(g) / 255.0f) * 5.0f))
        + std::round((float(b) / 255.0f) * 5.0f);

    return res;
}

void ImTui_ImplText_RenderDrawData(ImDrawData * drawData, ImTui::TScreen * screen) {
    // Avoid rendering when minimized, scale coordinates for retina displays (screen coordinates != framebuffer coordinates)
    int fb_width = (int)(drawData->DisplaySize.x * drawData->FramebufferScale.x);
    int fb_height = (int)(drawData->DisplaySize.y * drawData->FramebufferScale.y);

    if (fb_width <= 0 || fb_height <= 0) {
        return;
    }

    screen->resize(ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);
    screen->clear();

    // Will project scissor/clipping rectangles into framebuffer space
    ImVec2 clip_off = drawData->DisplayPos;         // (0,0) unless using multi-viewports
    ImVec2 clip_scale = drawData->FramebufferScale; // (1,1) unless using retina display which are often (2,2)

    // LLMFUN PATCH: clear the per-row triangle spans once per frame so all
    // triangles accumulate into the same buffer (drawTriangle only
    // initializes rows newly added by its resize).

    // Render command lists
    for (int n = 0; n < drawData->CmdListsCount; n++)
    {
        const ImDrawList* cmd_list = drawData->CmdLists[n];

        for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++)
        {
            const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];
            {
                ImVec4 clip_rect;
                clip_rect.x = (pcmd->ClipRect.x - clip_off.x) * clip_scale.x;
                clip_rect.y = (pcmd->ClipRect.y - clip_off.y) * clip_scale.y;
                clip_rect.z = (pcmd->ClipRect.z - clip_off.x) * clip_scale.x;
                clip_rect.w = (pcmd->ClipRect.w - clip_off.y) * clip_scale.y;

                if (clip_rect.x < fb_width && clip_rect.y < fb_height && clip_rect.z >= 0.0f && clip_rect.w >= 0.0f)
                {
                    // LLMFUN PATCH: quad-to-cell mapping. Every glyph quad
                    // emitted by RenderText is exactly 1 cell wide
                    // (quad-width-1 invariant) and carries the codepoint in
                    // vertex 1's color and the cell width in vertex 2's color
                    // (encoding contract, imgui_draw.cpp RenderText). The
                    // cell position is the average of the six quad vertices;
                    // the glyph occupies the cell containing its pen x
                    // (xx = trunc(avg) = trunc(pen x), the same rule used for
                    // yy). Do NOT re-add the legacy "+1" that used to shift
                    // all text one column right: it desynchronized text from
                    // the rects/caret/hit-testing coordinate system.
                    // The dedup heuristic below
                    // landing on the same cell to lastCharX + 1 so two glyphs never
                    // overwrite one cell. Vertices are clipped to
                    // clip_rect.z - 1 so content can never overwrite the
                    // scrollbar column. ch/chwidth/ch2 are taken from the
                    // encoded vertex colors (vertex 1 = codepoint, vertex 2
                    // = cell width, vertex 3 = continuation codepoint folded
                    // into the base — decoded below, guarded by
                    // col2 being a cell width); rect cells painted by
                    // drawTriangle() (scrollbar track/grab, window bg) set
                    // ch = ' ' and leave chwidth/ch2 = 0.
                    float lastCharX = -10000.0f;
                    float lastCharY = -10000.0f;

                    for (unsigned int i = 0; i < pcmd->ElemCount; i += 3) {
                        int vidx0 = cmd_list->IdxBuffer[pcmd->IdxOffset + i + 0];
                        int vidx1 = cmd_list->IdxBuffer[pcmd->IdxOffset + i + 1];
                        int vidx2 = cmd_list->IdxBuffer[pcmd->IdxOffset + i + 2];

                        auto pos0 = cmd_list->VtxBuffer[vidx0].pos;
                        auto pos1 = cmd_list->VtxBuffer[vidx1].pos;
                        auto pos2 = cmd_list->VtxBuffer[vidx2].pos;

                        pos0.x = std::max(std::min(float(clip_rect.z - 1), pos0.x), clip_rect.x);
                        pos1.x = std::max(std::min(float(clip_rect.z - 1), pos1.x), clip_rect.x);
                        pos2.x = std::max(std::min(float(clip_rect.z - 1), pos2.x), clip_rect.x);
                        pos0.y = std::max(std::min(float(clip_rect.w - 1), pos0.y), clip_rect.y);
                        pos1.y = std::max(std::min(float(clip_rect.w - 1), pos1.y), clip_rect.y);
                        pos2.y = std::max(std::min(float(clip_rect.w - 1), pos2.y), clip_rect.y);

                        auto uv0 = cmd_list->VtxBuffer[vidx0].uv;
                        auto uv1 = cmd_list->VtxBuffer[vidx1].uv;
                        auto uv2 = cmd_list->VtxBuffer[vidx2].uv;

                        auto col0 = cmd_list->VtxBuffer[vidx0].col;
                        auto col1 = cmd_list->VtxBuffer[vidx1].col;
                        auto col2 = cmd_list->VtxBuffer[vidx2].col;

                        if (uv0.x != uv1.x || uv0.x != uv2.x || uv1.x != uv2.x ||
                            uv0.y != uv1.y || uv0.y != uv2.y || uv1.y != uv2.y) {
                            int vvidx0 = cmd_list->IdxBuffer[pcmd->IdxOffset + i + 3];
                            int vvidx1 = cmd_list->IdxBuffer[pcmd->IdxOffset + i + 4];
                            int vvidx2 = cmd_list->IdxBuffer[pcmd->IdxOffset + i + 5];
                            // LLMFUN PATCH: vertex 3's color
                            // carries the continuation codepoint
                            // (VS16/combining mark folded into the base by
                            // RenderText; 0 = none). vvidx2 IS the quad's
                            // vertex 3 (index 5 of the quad's 6 indices), so
                            // this read is always in bounds here: the loop
                            // only reaches this branch for text quads, where
                            // i is the quad's first triangle (the trailing
                            // i += 3 below skips the second). Reading it
                            // inside the branch keeps the rect-triangle path
                            // (which has no vertex 3) free of any
                            // out-of-range index access.
                            auto col3 = cmd_list->VtxBuffer[vvidx2].col;

                            auto ppos0 = cmd_list->VtxBuffer[vvidx0].pos;
                            auto ppos1 = cmd_list->VtxBuffer[vvidx1].pos;
                            auto ppos2 = cmd_list->VtxBuffer[vvidx2].pos;

                            float x = ((pos0.x + pos1.x + pos2.x + ppos0.x + ppos1.x + ppos2.x)/6.0f);
                            float y = ((pos0.y + pos1.y + pos2.y + ppos0.y + ppos1.y + ppos2.y)/6.0f) + 0.5f;

                            if (std::fabs(y - lastCharY) < 0.5f && std::fabs(x - lastCharX) < 0.5f) {
                                x = lastCharX + 1.0f;
                                y = lastCharY;
                            }

                            lastCharX = x;
                            lastCharY = y;

                            int xx = (x);      // cell containing the pen x (no +1)
                            int yy = (y) + 0;
                            if (xx < clip_rect.x || xx >= clip_rect.z || yy < clip_rect.y || yy >= clip_rect.w) {
                            } else {
                                auto & cell = screen->data[yy*screen->nx + xx];
                                cell.ch = col1;
                                cell.chwidth = (uint8_t)col2;
                                // LLMFUN PATCH: decode the
                                // continuation codepoint (VS16/combining mark
                                // folded into the base by RenderText) into
                                // TCell::ch2. col3 is only meaningful for
                                // RenderText-encoded quads, i.e. quads whose
                                // col2 is a cell width (1 or 2); any other
                                // textured quad (e.g. the ImFont::RenderChar
                                // ellipsis path, which lacks the encoding)
                                // carries colors in these channels and must
                                // not leak a garbage continuation. Width-0
                                // codepoints never emit a quad, so text
                                // cells always decode chwidth >= 1.
                                cell.ch2 = (col2 >= 1 && col2 <= 2) ? (uint32_t)col3 : 0;
                                cell.fg = rgbToAnsi256(col0, false);
                            }
                            i += 3;
                        } else {
                            drawTriangle(pos0, pos1, pos2, rgbToAnsi256(col0, true), screen);
                        }
                    }
                }
            }
        }
    }

}

bool ImTui_ImplText_Init() {
    // LLMFUN PATCH (imtui): opt this imgui build into the imtui text-backend
    // behavior (UTF-8 vertex-color encoding, unit-cell glyph quads, caret and
    // arrow tuning). A GPU backend (e.g. OpenGL) sharing this imgui build never
    // sets the flag and gets unmodified upstream behavior — see the declaration
    // in imgui.h.
    ImTui_TextEncodingActive = true;
    ImGui::GetStyle().Alpha                   = 1.0f;
    ImGui::GetStyle().WindowPadding           = ImVec2(0.5f, 0.0f);
    ImGui::GetStyle().WindowRounding          = 0.0f;
    ImGui::GetStyle().WindowBorderSize        = 0.0f;
    ImGui::GetStyle().WindowMinSize           = ImVec2(4.0f, 2.0f);
    ImGui::GetStyle().WindowTitleAlign        = ImVec2(0.0f, 0.0f);
    ImGui::GetStyle().WindowMenuButtonPosition= ImGuiDir_Left;
    ImGui::GetStyle().ChildRounding           = 0.0f;
    ImGui::GetStyle().ChildBorderSize         = 0.0f;
    ImGui::GetStyle().PopupRounding           = 0.0f;
    ImGui::GetStyle().PopupBorderSize         = 0.0f;
    ImGui::GetStyle().FramePadding            = ImVec2(1.0f, 0.0f);
    ImGui::GetStyle().FrameRounding           = 0.0f;
    ImGui::GetStyle().FrameBorderSize         = 0.0f;
    ImGui::GetStyle().ItemSpacing             = ImVec2(1.0f, 0.0f);
    ImGui::GetStyle().ItemInnerSpacing        = ImVec2(1.0f, 0.0f);
    ImGui::GetStyle().TouchExtraPadding       = ImVec2(0.5f, 0.0f);
    ImGui::GetStyle().IndentSpacing           = 1.0f;
    ImGui::GetStyle().ColumnsMinSpacing        = 1.0f;
    // LLMFUN PATCH: scrollbars must span a full cell. The upstream 0.5-cell
    // strip (x = [col+0.5, col+1)) maps through nearbyint to zero or one cell
    // depending only on the column parity, so the vertical scrollbar of an
    // even-right-edge child never painted (test_utf8_grid check (d))) and the
    // chat log's scrollbar was invisible. A full cell = one deterministic
    // column.
    ImGui::GetStyle().ScrollbarSize           = 1.0f;
    ImGui::GetStyle().ScrollbarRounding       = 0.0f;
    // LLMFUN PATCH: a 0.1-cell minimum grab is sub-cell (a 0-1 row sliver);
    // one cell guarantees the thumb can actually be painted.
    ImGui::GetStyle().GrabMinSize             = 1.0f;
    ImGui::GetStyle().GrabRounding            = 0.0f;
    ImGui::GetStyle().TabRounding             = 0.0f;
    ImGui::GetStyle().TabBorderSize           = 0.0f;
    ImGui::GetStyle().ColorButtonPosition     = ImGuiDir_Right;
    ImGui::GetStyle().ButtonTextAlign         = ImVec2(0.5f,0.0f);
    ImGui::GetStyle().SelectableTextAlign     = ImVec2(0.0f,0.0f);
    ImGui::GetStyle().DisplayWindowPadding    = ImVec2(0.0f,0.0f);
    ImGui::GetStyle().DisplaySafeAreaPadding  = ImVec2(0.0f,0.0f);
    ImGui::GetStyle().CellPadding             = ImVec2(1.0f,0.0f);
    ImGui::GetStyle().MouseCursorScale        = 1.0f;
    ImGui::GetStyle().AntiAliasedLines        = false;
    ImGui::GetStyle().AntiAliasedFill         = false;
    ImGui::GetStyle().CurveTessellationTol    = 1.25f;

    ImGui::GetStyle().Colors[ImGuiCol_WindowBg]         = ImVec4(0.15, 0.15, 0.15, 1.0f);
    ImGui::GetStyle().Colors[ImGuiCol_TitleBg]          = ImVec4(0.35, 0.35, 0.35, 1.0f);
    ImGui::GetStyle().Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.15, 0.15, 0.15, 1.0f);
    ImGui::GetStyle().Colors[ImGuiCol_TextSelectedBg]   = ImVec4(0.75, 0.75, 0.75, 0.5f);
    ImGui::GetStyle().Colors[ImGuiCol_NavHighlight]     = ImVec4(0.00, 0.00, 0.00, 0.0f);

    ImFontConfig fontConfig;
    fontConfig.GlyphMinAdvanceX = 1.0f;
    fontConfig.SizePixels = 1.00f;
    // 1.92's AddFontDefault() dispatches to the scalable vector font at normal
    // context font sizes; imtui needs the 1px ProggyClean bitmap and the legacy
    // atlas path (RendererHasTextures unset, GetTexDataAsRGBA32 below).
    ImGui::GetIO().Fonts->AddFontDefaultBitmap(&fontConfig);

    // Build atlas
    unsigned char* tex_pixels = NULL;
    int tex_w, tex_h;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&tex_pixels, &tex_w, &tex_h);

    return true;
}

void ImTui_ImplText_Shutdown() {
    // LLMFUN PATCH (imtui): reset the text-backend opt-in flag. One imgui build
    // now serves both the imtui text path and a pixel backend (OpenGL) in the
    // same process; a stale `true` would corrupt pixel rendering (see the
    // declaration in imgui.h and the guarded patches in imgui_draw.cpp /
    // imgui_widgets.cpp / imgui.cpp).
    ImTui_TextEncodingActive = false;
}

void ImTui_ImplText_NewFrame() {
}
