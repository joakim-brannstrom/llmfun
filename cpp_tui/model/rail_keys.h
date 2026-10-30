/// The rail's row keys: the characters the rail reads itself (`views/rail.cpp`), in one table.
///
/// They are not `Action` rows (plan/key_gate.md F2: they arrive as characters, not as key
/// events), so the registry cannot describe them - and the generated key table
/// (`doc/ui_keys.md`) must not describe them from prose either, or changing `d` to `x` would
/// leave the document advertising `d`. The rail reads `kRailRowKeys[i].key` through
/// `ui::takeCharacter()`, and `model::keymap_doc.cpp` prints the same table: one source.
#pragma once

#include <cstddef>

namespace llmfun::tui::model {

/// One row key: the character and what it does, in the words the document uses.
struct RailKey {
    char key;
    const char* action;
};

/// The `/` key is not here: it is `takeSearchKey()`'s (the design's "focus search from anywhere
/// except the composer"), not a row key, and it works while no row is targeted.
inline constexpr char kRailSearchKey = '/';
inline constexpr char kRailMenuKey = 'm';
inline constexpr char kRailRenameKey = 'r';
inline constexpr char kRailDeleteKey = 'd';
inline constexpr char kRailPinKey = 'p';

/// The row keys in the order the document lists them. `Enter` in the search field is the field's
/// own key, not a row key.
inline constexpr RailKey kRailRowKeys[] = {
    {kRailMenuKey, "open the row menu"},
    {kRailRenameKey, "rename the row"},
    {kRailDeleteKey, "delete the row, in one press (the guard is the aimed-at row)"},
    {kRailPinKey, "pin or unpin the row"},
};
inline constexpr std::size_t kRailRowKeyCount = sizeof(kRailRowKeys) / sizeof(kRailRowKeys[0]);

} // namespace llmfun::tui::model
