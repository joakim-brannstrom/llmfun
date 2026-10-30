/// Chrome constants of the pixel profile: the GUI window geometry (D-GEOM), and
/// the declaration of the one call that dresses the ImGui style.
///
/// The *metrics* (padding, spacing, radii, hairlines) are no longer here: task 20
/// moved them into the token module, where `ui::space()`, `ui::radius()` and the
/// profile's own hairline are the only source (D-THEME). `test_gui_window` pins
/// the resulting pixel values as numbers, so a token change is still a reviewed
/// diff and not a silent re-style.
///
/// The window geometry stays, because it is not a token: 1280x820 is the size the
/// design opens at and 640x480 is the floor a drag cannot cross. The text grid's
/// floor (40x15 cells) is a different unit and stays in tui.cpp.
#pragma once

namespace llmfun::tui {

// A fresh GUI profile opens at 1280x820 and cannot be dragged below 640x480.
inline constexpr int DefaultGuiWindowWidth = 1280;
inline constexpr int DefaultGuiWindowHeight = 820;
inline constexpr int MinGuiWindowWidth = 640;
inline constexpr int MinGuiWindowHeight = 480;

/// Apply the pixel profile's chrome to the live ImGui style: window padding, item
/// and frame spacing, the three corner radii, the scrollbar and the hairline
/// border sizes - every value a token, authored at 100 % scale (D-THEME).
///
/// Pixel profile only. In the text grid one style unit is one terminal cell, so
/// pixel spacing would multiply every gap by ~8 and break each cell-exact
/// layout; ImTui_ImplText_Init() owns those values instead. The call therefore
/// sits in GuiBackend::init() rather than in applyUiTheme(), which both profiles
/// run and which several harnesses call before their backend exists.
///
/// The full-window root keeps square corners (tui.cpp pushes WindowRounding 0
/// around it): a 6 px radius on a 1280x820 surface only draws four clear-colour
/// wedges inside a square window frame.
void applyGuiChromeMetrics();

} // namespace llmfun::tui
