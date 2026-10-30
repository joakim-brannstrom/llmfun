/// The markdown host: the rich_md context's lifecycle and the agent stream's bookkeeping.
///
/// These definitions used to live in `tui_chat.cpp`, the legacy chat/log renderer's
/// translation unit - they are shared by every render profile (the text grid arms its
/// 1-cell bakes before `ImTui_ImplText_Init()`, the pixel profile loads lazily), so they
/// moved here when that renderer was deleted (task 41).
#include "tui.h"
#include "ui/markdown.h"
#include "ui/tokens.h"

#include "imgui/imgui_internal.h"
#include "imgui_rich_md/rich_md.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace llmfun::tui {

void AgentStream::finished() {
    if (stream.content.empty() && stream.thinking.empty()) {
        stream = AgentStreamMessage{};
        return;
    }

    updateCnt++;
    messages.emplace_back(stream);
    if (messages.size() > MaxMessages) {
        messages.erase(messages.begin());
    }
    stream = AgentStreamMessage{};
}

static size_t countNewLines(const std::string& str) {
    return std::count(str.begin(), str.end(), '\n');
}

void initMarkdownTextMode(TuiState& state) {
    RichMd::MarkdownOptions options;
    options.fontOptions.regularSize = 1.0f;
    // 1-cell rows: the terminal grid has one row per line, so headers must not
    // grow (a larger header would eat rows the chat layout does not budget).
    for (int i = 0; i < 6; ++i)
        options.fontOptions.headerSizeFactors[i] = 1.0f;
    options.hardSoftBreaks = true; // newline = one row (chat messages, not docs)
    // The selection policy is the wrapper's (ui/markdown.h): a cell has no mouse
    // selection here, and `Ctrl+C` is the quit chord on the grid.
    options.selectableText = ui::markdownSelectable(/*textGrid=*/true);
    // Link click copies the URL to the clipboard (behavior preserved from the
    // previous markdown library's link callback); images render as nothing
    // (no asset reads, no texture attempts, no stderr warning on the
    // grid).
    options.callbacks.OnOpenLink = [](std::string url) { ImGui::SetClipboardText(url.c_str()); };
    options.callbacks.OnImage = nullptr;
    state.mdContext = RichMd::CreateContext(options);
    // The wrapper renders through this context and styles it from the tokens.
    ui::setMarkdownContext(state.mdContext);
    // The table bridge's renderers, on this context only (design §6.8, task 37): the grid
    // draws a bridged table, the pixel profile keeps rich_md's native ones.
    ui::registerTableFenceRenderers();
    // Adds the rich fonts NOW: this must precede ImTui_ImplText_Init() (which
    // the text backend runs right after us), because that call builds and
    // locks the atlas — a font added to a locked atlas would resolve its
    // glyph queries to the closest existing bake (16px -> 1/16 scale) and
    // render as invisible 1px slivers.
    RichMd::GetFontLoaderFunction()();
    ImGuiIO& io = ImGui::GetIO();
    // Pre-create the EXACT 1.0f bakes rich_md will push on every frame, and
    // preload their glyph ranges, before the first frame (during a frame the
    // atlas is locked, so a missing bake would resolve to the closest
    // existing one, and unloaded glyphs would come back as an invisible
    // synthetic fallback with no quad emitted).
    for (ImFont* f : io.Fonts->Fonts) {
        ImFontBaked* baked = f->GetFontBaked(1.0f);
        baked->FindGlyph(f->FallbackChar);
        baked->FindGlyph((ImWchar)' ');
        for (ImWchar c = 0x20; c <= 0xFF; ++c)
            baked->FindGlyph(c);
    }
    // Everything rich_md draws with comes from the wrapper (ui/markdown.h): the
    // tokens for this profile, written into RichMd::Style - links, inline code,
    // the quote and admonition colours, the block/header gaps, and the grid's own
    // corrections (sub/sup one cell tall; both bars one cell wide and strictly
    // left of the content, where rich_md's 2-3 px default would erase the
    // content's first cells: "> `Foo` bar" -> "  oo bar", "> [!NOTE]" -> "OTE").
    ui::applyMarkdownTheme(ui::markdownThemeFromTokens(/*textGrid=*/true));
}

void finishMarkdownTextMode(TuiState& state) {
    ImGuiIO& io = ImGui::GetIO();
    // Pin io.FontDefault to the imtui 1px font, found BY NAME: matching on
    // LegacySize==1.0f alone is ambiguous (the rich code font is also baked
    // at 1px in this profile), and the name is set by AddFontDefaultBitmap.
    ImFont* imtuiFont = nullptr;
    for (ImFont* f : io.Fonts->Fonts)
        if (f->Sources[0]->Name && std::string(f->Sources[0]->Name) == "ProggyClean.ttf") {
            IM_ASSERT(imtuiFont == nullptr);  // exactly one match
            IM_ASSERT(f->LegacySize == 1.0f); // and it is the 1px font
            imtuiFont = f;
        }
    IM_ASSERT(imtuiFont != nullptr);
    // Pin: keeps the whole UI (not just the markdown) at 1-cell metrics.
    // No fonts may be added after this call: they would render at their own
    // size against the locked atlas, outside the 1-cell grid.
    io.FontDefault = imtuiFont;
}

// The GUI's UI font is the pixel backend's business, not markdown's: the backend's
// `configureUiFont()` (tui_backend_gui.cpp) walks the family chain and installs the
// embedded Roboto as io.FontDefault before the first frame, so markdown neither
// picks nor replaces the family the rest of the UI draws with.
void initMarkdownGuiMode(TuiState& state) {
    RichMd::MarkdownOptions options;
    // The transcript is body text (design §4.6): rich_md's own default is 16 px,
    // which is not one of the six steps and would leave the messages a size off the
    // chrome around them. The header factors stay rich_md's - a heading is display
    // or title, expressed as a multiple of the body step.
    options.fontOptions.regularSize = ui::typeSize(int(ui::TypeStep::body));
    options.hardSoftBreaks = true; // newline = one line (chat messages)
    options.selectableText = ui::markdownSelectable(/*textGrid=*/false);
    options.callbacks.OnOpenLink = [](std::string url) { ImGui::SetClipboardText(url.c_str()); };
    options.callbacks.OnImage = nullptr; // no image asset reads, in both profiles
    // Lazy fonts (loaded at the first Render): the GL3 backend supports
    // dynamic texture updates, so no warm-up is needed here.
    state.mdContext = RichMd::CreateContext(options);
    // The wrapper owns the context from here on, and the tokens (not rich_md's
    // automatic colour shifts) are what markdown draws with.
    ui::setMarkdownContext(state.mdContext);
    ui::applyMarkdownTheme(ui::markdownThemeFromTokens(/*textGrid=*/false));
}

void destroyMarkdown(TuiState& state) {
    // The wrapper's slot is one per process, but a state is not: destroying a
    // state that never had a context (every headless one) must not clear the
    // pointer of the state that owns it, or that state silently falls back to
    // plain text for the rest of its life. Clear it only when it is ours - then
    // the wrapper forgets the context before it is freed, so no render can draw
    // through a dangling pointer, and the first frame into a new context (a
    // backend switch makes one) re-applies the style instead of trusting the old
    // cache.
    if (state.mdContext && ui::markdownContext() == state.mdContext)
        ui::setMarkdownContext(nullptr);
    if (state.mdContext) {
        // Frees the rich textures: must run while the rendering backend is
        // still alive (the tuiDestroyState caller guarantees that order).
        RichMd::DestroyContext(state.mdContext);
        state.mdContext = nullptr;
    }
}

} // namespace llmfun::tui
