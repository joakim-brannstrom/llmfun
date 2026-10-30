/// `gen_ui_keys` - print `doc/ui_keys.md` from the binding registry.
///
/// Usage: `./gen_ui_keys > doc/ui_keys.md` (or `tools/gen_ui_keys.sh`, which does that and
/// leaves the file untouched when nothing changed).
#include <cstdio>
#include <string>

#include "model/keymap_doc.h"

int main() {
    const std::string text = llmfun::tui::model::uiKeysMarkdown();
    std::fwrite(text.data(), 1, text.size(), stdout);
    return 0;
}
