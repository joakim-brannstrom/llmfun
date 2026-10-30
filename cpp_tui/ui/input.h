/// Printable input, one home for every view that acts on a *character* rather than
/// on a chord (plan/key_gate.md F2, task 26).
///
/// The text grid delivers printable input as characters only: `IsKeyPressed(Y)` is
/// never true in the terminal, and the ncurses backend queues the byte as an
/// `InputQueueCharacters` entry. A row key therefore cannot be a keymap row - the
/// registry's chords are key events - so it is read from the character queue and
/// *consumed*, which is the shipped shape of task 7's `m` and task 23's rail keys
/// (`m`, `r`, `d`, `p`, `/`). Task 26's `y` (copy the focused block) is the same
/// shape, and this header is what keeps it one implementation instead of two
/// copies of a fifteen-line loop.
///
/// The plain-key rule (D-KEYMAP) is part of the contract, not of the caller: a text
/// field that owns the keyboard keeps its characters, so typing `y` in the composer
/// stays typing.
///
/// Layer: `ui/` only - imgui and nothing else, no `tui.h`, no view header.
#pragma once

namespace llmfun::tui::ui {

/// Take `lower` (or its upper-case spelling) out of this frame's character queue.
///
/// True when the character was queued and has been removed, so the caller's
/// handling of it cannot also reach a widget. False while a text field owns the
/// keyboard (`io.WantTextInput`) or when the character is not there - the queue is
/// then left exactly as it was.
///
/// Only a letter has a second spelling: `'a' - 'a' + 'A'` on a non-letter produces
/// a *control* codepoint (`'/'` -> 0x0F), which would let a queued `Ctrl+O` count as
/// a `/` on a backend that queues control characters.
bool takeCharacter(char lower);

/// `takeCharacter()` without the text-field guard, for a surface that has just taken
/// the keyboard off *its own* field.
///
/// The guard above is process-global and goes stale: after a field gives the keyboard
/// up, ImGui's nav can re-activate it (and keep `WantTextInput` set) for a couple of
/// frames, so a plain key pressed in that window would be neither typed nor handled.
/// A caller may skip the guard only when it knows the field the flag describes is its
/// own and is released - the settings sheet's `/` is the case this exists for.
bool takeCharacterAfterRelease(char lower);

} // namespace llmfun::tui::ui
