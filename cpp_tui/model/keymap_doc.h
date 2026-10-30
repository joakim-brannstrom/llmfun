/// The keymap document (`doc/ui_keys.md`): one table generated from the binding registry
/// (model/keymap.*), so the document cannot claim a chord the build does not bind.
///
/// Generation is the rule (D-KEYGATE's "no binding ships without a row"): `tools/gen_ui_keys`
/// prints this text and `./test_ui_keys` compares it with the committed file, so a binding that
/// changes without regenerating the document fails the build's tests rather than the user's
/// first press.
///
/// The help page (`views/help.cpp`, task 42) reads the same binding table and the same chord
/// spelling (`model::chordLabel()`, declared in keymap.h), so its key section is this document's
/// table in another shape and cannot drift from it either.
#pragma once

#include <string>

namespace llmfun::tui::model {

/// The whole document, markdown, ending in a newline. Deterministic: no clock, no display, no
/// ImGui context (the chord names are spelled by `chordLabel()`, not by
/// `ImGui::GetKeyChordName`).
std::string uiKeysMarkdown();

} // namespace llmfun::tui::model
