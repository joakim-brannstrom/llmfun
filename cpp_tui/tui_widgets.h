/// Shared TUI render helper: a separator row.
#pragma once

#include <array>
#include <string>
#include <string_view>

#include "imtui/imtui.h"

#include "imgui/imgui_internal.h"

namespace llmfun::tui {

// Per-TU private copy (internal linkage): each including translation unit gets
// its own `static` definition, so this adds no new exported symbol.
static void renderSeparator(const std::string& prefix, std::string_view ch, int n, bool disabled) {
    static std::array<char, 256> buf{};
    if (n > 255)
        n = 255;
    if (n < 0)
        n = 0;
    if (n > prefix.size())
        n -= prefix.size();
    for (int i = 0; i < n; i++) {
        buf[i] = ch[0];
    }
    buf[n] = 0;
    if (disabled) {
        ImGui::TextDisabled("%s%s", prefix.c_str(), buf.data());
    } else {
        ImGui::Text("%s%s", prefix.c_str(), buf.data());
    }
}

} // namespace llmfun::tui
