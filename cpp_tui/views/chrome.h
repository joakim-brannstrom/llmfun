/// The words the shell's chrome shares between views.
///
/// One signal, one string. A literal repeated in two views is a signal that can drift into
/// two names for one fact - and no golden would catch it, because each view's golden only
/// pins its own row. Anything shared by more than one *view* belongs here; a word only one
/// view draws stays in that view's header.
///
/// Layering: this header includes nothing and declares only constants, so any view may
/// include it (and `ui/`, `model/` and the store adapter need not).
#pragma once

namespace llmfun::tui::views {

/// The Raw Log is open (D-RAWLOG): the badge the top bar carries, and the now bar in
/// compact mode - where the top bar is folded away, so this row is the only place the
/// signal can be. The design writes the word in capitals, and it is a *word* beside a
/// coloured chip rather than a mark, so a reader without colour still knows the developer
/// log is on.
inline constexpr char kRawBadge[] = "RAW";

} // namespace llmfun::tui::views
