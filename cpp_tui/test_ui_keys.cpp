// test_ui_keys.cpp
//
// Headless gate for `doc/ui_keys.md`: the document is *generated* from the binding registry
// (`model::uiKeysMarkdown()`), so the committed file has to match what the registry says today.
// A binding added, removed or re-scoped without regenerating the document fails here instead of
// shipping a key table the build does not honour (D-KEYGATE's "no binding ships without a row",
// read in both directions: no row without a binding either).
//
// The document's path comes from TEST_UI_KEYS_DOC (CMake sets it to the source tree), so the
// binary can run from the build directory like every other harness.
//
// Exit codes: 0 = the file matches, 1 = it differs, 2 = the file could not be read.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "model/keymap_doc.h"

namespace {

int g_failures = 0;

void fail(const std::string& what) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++g_failures;
}

/// The first line where `want` and `got` differ, for a diagnostic that names the fix.
void reportFirstDifference(const std::string& want, const std::string& got) {
    std::size_t wantLine = 1;
    std::size_t gotLine = 1;
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < want.size() && j < got.size()) {
        if (want[i] != got[j]) {
            std::fprintf(stderr, "first difference at line %zu (regenerated) / %zu (file)\n",
                         wantLine, gotLine);
            const std::size_t fromW =
                want.rfind('\n', i) == std::string::npos ? 0 : want.rfind('\n', i) + 1;
            const std::size_t fromG =
                got.rfind('\n', j) == std::string::npos ? 0 : got.rfind('\n', j) + 1;
            std::fprintf(stderr, "  regenerated: %s\n",
                         want.substr(fromW, want.find('\n', fromW) - fromW).c_str());
            std::fprintf(stderr, "  file:        %s\n",
                         got.substr(fromG, got.find('\n', fromG) - fromG).c_str());
            return;
        }
        if (want[i] == '\n')
            ++wantLine;
        if (got[j] == '\n')
            ++gotLine;
        ++i;
        ++j;
    }
}

} // namespace

int main() {
    const char* path = TEST_UI_KEYS_DOC;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot read %s\n", path);
        return 2;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string onDisk = buffer.str();
    const std::string generated = llmfun::tui::model::uiKeysMarkdown();

    if (onDisk != generated) {
        fail(std::string(path) + " does not match the binding registry");
        reportFirstDifference(generated, onDisk);
        std::fprintf(stderr, "run tools/gen_ui_keys.sh to regenerate it\n");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("OK: %s matches the registry (%zu bytes)\n", path, generated.size());
    return 0;
}
