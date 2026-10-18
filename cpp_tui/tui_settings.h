#pragma once

#include <string>

namespace llmfun::tui {

/// GUI view settings persisted in imgui2.conf (a sibling of imgui2.ini);
/// windowWidth/windowHeight 0 = "no persisted size", fontScaleMain defaults to 1.0.
struct TuiSettings {
    float fontScaleMain = 1.0f;
    int windowWidth = 0;
    int windowHeight = 0;
};

/// Derive the settings path from the imgui ini filename by replacing a
/// trailing ".ini" (exact case) with ".conf" ("imgui2.ini" -> "imgui2.conf";
/// any other name gets ".conf" appended). Empty input returns "".
std::string settingsPath(const std::string& iniFilename);

/// Load settings from iniFilename's derived path. Line format "Key=Value",
/// split at the first '='; lines without '=' and unknown keys are skipped.
/// Keys are matched case-sensitively and NOT trimmed (a hand-edited
/// " WindowWidth=5" line is an unknown key). Per key, any parse failure
/// (strict strtol/strtof, ERANGE, NaN, out of range) keeps the current
/// value; parsed values are clamped (WindowWidth/WindowHeight to
/// [1, 32767], FontScaleMain to [0.5, 3.0]; std::clamp passes NaN through,
/// so the finite check is explicit).
/// Missing file or empty iniFilename: no-op (the struct keeps its values).
/// Values never reach shell or format strings.
void loadTuiSettings(TuiSettings& settings, const std::string& iniFilename);

/// Save all settings to iniFilename's derived path, one "Key=Value" line per
/// setting, trailing newline, fopen "wt" (text mode):
///   WindowWidth=%d\n   (written only when > 0)
///   WindowHeight=%d\n  (written only when > 0)
///   FontScaleMain=%.2f\n  (always; up to 2 decimals)
/// Empty iniFilename: no-op, no file created. Best-effort: write
/// failures are ignored silently (returns void; nothing logs).
void saveTuiSettings(const TuiSettings& settings, const std::string& iniFilename);

struct TuiState;

/// Render the Options window (View -> Options). No-op while optionsOpen is
/// false; the window's X sets the flag false. ImGui-dependent; compiled out
/// of the headless test build (TUI_SETTINGS_HEADLESS_TEST).
void renderOptionsWindow(TuiState& state);

} // namespace llmfun::tui
