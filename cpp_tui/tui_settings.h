#pragma once

namespace llmfun::tui {

struct TuiState;

/// Render the Options window (View -> Options). No-op while optionsOpen is
/// false; the window's X sets the flag false. It edits `state.ui.prefs` (the
/// one `model::ShellPrefs`, which `ui.conf` persists) and never touches a
/// file - the frame layer's debounce and quit/shutdown saves persist.
void renderOptionsWindow(TuiState& state);

} // namespace llmfun::tui
