/// Implementations of the shared TUI core utilities (tui_common.h).
#include "tui_common.h"

#include <algorithm>
#include <cctype>

namespace llmfun::tui {

bool isWhitespaceOnly(const std::string& s) {
    if (s.empty())
        return true;

    bool allTrue = true;
    for (auto c : s) {
        // Cast to unsigned char: std::isspace is UB for negative values,
        // and titles may contain arbitrary UTF-8 bytes.
        allTrue = allTrue && (std::isspace(static_cast<unsigned char>(c)) || c == '\0');
    }
    return allTrue;
}

void textUnformattedMultiline(std::string_view text) {
    auto begin = text.begin();
    auto end = text.end();
    while (begin != end) {
        auto next = std::find(begin, end, '\n');
        ImGui::TextUnformatted(begin, next);
        begin = (next != end) ? next + 1 : end;
    }
}

void textUnformattedMultiline(ImGui::MarkdownConfig& mdConfig, std::string_view text,
                              bool renderMarkdown) {
    if (renderMarkdown) {
        ImGui::Markdown(text.data(), text.length(), mdConfig);
    } else {
        textUnformattedMultiline(text);
    }
}

} // namespace llmfun::tui
