/// The metrics module (D-METRICS, design §3.4): the design's lengths, resolved
/// for one render profile and one scale.
///
/// Two tables, one per profile, because the two profiles measure different
/// things: the text grid counts cells and cannot round, the GUI counts pixels and
/// has a font scale. Everything downstream sees only the resolved `Metrics`, so
/// "which profile am I" is answered in this file and nowhere else.
///
/// `metrics.h` states where each number comes from and why; this file holds the
/// numbers themselves.
#include "ui/metrics.h"

#include "ui/tokens.h"

#include <algorithm>
#include <cmath>

namespace llmfun::tui::ui {
namespace {

/// The six fixed region sizes plus the shared lengths, in the profile's own unit.
struct Design {
    float gu;
    float row;
    float pad1;
    float pad2;
    float pad3;
    float radius;
    float border;
    float topBarH;
    float railW;
    float railCollapsedW;
    float inspectorW;
    float nowBarH;
};

/// Text grid: the design's cell counts (design §4.15 token steps rounded to the
/// cell grid, and the anatomy tables of §4.2-§4.8). Every value is an integer by
/// construction - a cell is the smallest unit a terminal has - and the radius is
/// zero because a rounded corner needs a curve a cell cannot draw.
constexpr Design kGridDesign{
    /*gu*/ 1.0f,
    /*row*/ 1.0f,
    /*pad1*/ 1.0f,
    /*pad2*/ 2.0f,
    /*pad3*/ 3.0f,
    /*radius*/ 0.0f,
    /*border*/ 1.0f,
    /*topBarH*/ 1.0f,
    /*railW*/ 26.0f,
    /*railCollapsedW*/ 7.0f,
    /*inspectorW*/ 28.0f,
    /*nowBarH*/ 1.0f,
};

/// GUI at 100 % scale. Spacing and radii are read from the token table
/// (`space`/`radius`) so no distance here is a fresh literal (D-THEME); the
/// region sizes are not token steps - they are the design's anatomy - and stay
/// named constants of this file.
Design guiDesign() {
    return Design{
        /*gu*/ 8.0f,
        /*row*/ 24.0f,
        /*pad1*/ space(int(Space::base)),
        /*pad2*/ space(int(Space::comfy)),
        /*pad3*/ space(int(Space::section)),
        /*radius*/ radius(int(Radius::large)),
        /*border*/ 1.0f,
        /*topBarH*/ 48.0f,
        /*railW*/ 208.0f,
        /*railCollapsedW*/ 56.0f,
        /*inspectorW*/ 224.0f,
        /*nowBarH*/ 28.0f,
    };
}

/// Composer text rows: one when empty (plus the meta row), eight when full
/// (design §4.6, auto-grow 1-8).
constexpr float kComposerTextRowsMin = 1.0f;
constexpr float kComposerTextRowsMax = 8.0f;
/// The meta row (Send/Stop, meter, model) and the container's two hairlines.
constexpr float kComposerMetaRows = 1.0f;

/// A scale factor that cannot poison the layout: a NaN, an infinity or a
/// non-positive value (a hand-edited setting, a monitor query that answered 0)
/// means "100 %", never "multiply by it". Two legitimate factors can still
/// overflow the product (1e20 x 1e20), which would round to a zero length, so the
/// product is capped at 800 % - wider than the settings' own 50-300 % range.
constexpr float kMaxScale = 8.0f;

float saneScale(float scale) { return (std::isfinite(scale) && scale > 0.0f) ? scale : 1.0f; }

/// The one scale the pixel profile applies, in one place, so both factors are
/// sanitised and the product is bounded before any length is multiplied by it.
float pixelScale(float fontScale, float dpiScale) {
    return std::min(saneScale(fontScale) * saneScale(dpiScale), kMaxScale);
}

} // namespace

float roundCells(float v) {
    if (!std::isfinite(v))
        return 0.0f;
    // Round-half-down, i.e. ceil(v - 0.5): 0.5 -> 0, 1.5 -> 1, 2.5 -> 2, while an
    // already whole length is unchanged and 0.51 still rounds up to 1. The usual
    // floor(v + 0.5) answers 1 for a half, which hands a 2.5-cell budget a third
    // cell it does not have.
    // `+ 0.0f` folds the -0.0 ceil() returns for v < 0.5 into +0.0, so a zero
    // length compares equal to zero.
    return std::ceil(v - 0.5f) + 0.0f;
}

Metrics metricsFor(bool textGrid, float fontScale, float dpiScale) {
    const Design design = textGrid ? kGridDesign : guiDesign();
    // The grid is measured in cells the terminal chooses: the text scale and the
    // monitor scale are properties of the pixel profile.
    const float scale = textGrid ? 1.0f : pixelScale(fontScale, dpiScale);

    Metrics m;
    m.textGrid = textGrid;
    m.gu = roundCells(design.gu * scale);
    m.row = roundCells(design.row * scale);
    m.pad1 = roundCells(design.pad1 * scale);
    m.pad2 = roundCells(design.pad2 * scale);
    m.pad3 = roundCells(design.pad3 * scale);
    m.radius = roundCells(design.radius * scale);
    m.border = roundCells(design.border * scale);
    m.topBarH = roundCells(design.topBarH * scale);
    m.railW = roundCells(design.railW * scale);
    m.railCollapsedW = roundCells(design.railCollapsedW * scale);
    m.inspectorW = roundCells(design.inspectorW * scale);
    m.nowBarH = roundCells(design.nowBarH * scale);
    // Both composer heights are the same anatomy in both profiles - text rows +
    // meta row + the two container hairlines - built from the *resolved* row and
    // border rather than from the 100 % total times the scale: at 150 % the rows
    // grow (36 px) while the hairline stays 1 px, so scaling 50 px would answer
    // 75 for a container whose parts add up to 74. Grid: 4 and 11 cells.
    m.composerMinH =
        roundCells((kComposerTextRowsMin + kComposerMetaRows) * m.row + 2.0f * m.border);
    m.composerMaxH =
        roundCells((kComposerTextRowsMax + kComposerMetaRows) * m.row + 2.0f * m.border);
    return m;
}

} // namespace llmfun::tui::ui
