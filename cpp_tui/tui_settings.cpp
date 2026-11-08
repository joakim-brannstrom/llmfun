/// The Options window (View -> Options); see tui_settings.h.
#include "tui_settings.h"

#include "model/settings.h"

#include "imgui.h"
#include "tui.h"

#include <cstdlib>

namespace llmfun::tui {

// Text-scale UX constants: the load-acceptance range is the model's
// kFontScaleMin/kFontScaleMax (settings.h, the one owner of the rule); the
// slider works in percent (percent = fontScaleMain * PercentPerUnit), with
// Min/MaxFontScalePercent as its range and PercentRoundHalf for the readout.
// The quantize step is the model's kFontScaleStepPercent.
constexpr int MinFontScalePercent = 50;
constexpr int MaxFontScalePercent = 300;
constexpr float PercentPerUnit = 100.0f;
constexpr float PercentRoundHalf = 0.5f;

// Fixed width of the Options window's text-scale slider: the content fit
// measures the slider item, so a window-dependent width would feed back into
// the next fit and make the window drift.
constexpr float OptionsSliderWidthPx = 200.0f;

// Compile-time tie between the percent constants and the model's scale
// constants: a silent divergence would let the slider save a value the load
// check then rejects at the next start (a reset to 1.0 with no diagnostic).
static_assert(MinFontScalePercent ==
              static_cast<int>(model::kFontScaleMin * PercentPerUnit + PercentRoundHalf));
static_assert(MaxFontScalePercent ==
              static_cast<int>(model::kFontScaleMax * PercentPerUnit + PercentRoundHalf));

void renderOptionsWindow(TuiState& state) {
    if (!state.optionsOpen)
        return;
    // Fit the window to its content on the first open and on every text-scale
    // change: the extent is measured after every widget (the slider has a
    // fixed width, so the measurement is window-independent) and applied one
    // frame later with a 30 px right/bottom margin. Every other frame is left
    // alone, so user resizing sticks (and persists via imgui2.ini).
    constexpr float FitMarginPx = 30.0f;
    if (state.optionsFitScale != state.ui.prefs.fontScaleMain &&
        state.optionsContentSize.x > 0.0f && state.optionsContentSize.y > 0.0f) {
        state.optionsFitScale = state.ui.prefs.fontScaleMain;
        // Keep the window on-screen even at the 300 % maximum scale.
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        ImVec2 fit(state.optionsContentSize.x + FitMarginPx,
                   state.optionsContentSize.y + FitMarginPx);
        if (display.x > 0.0f && fit.x > display.x)
            fit.x = display.x;
        if (display.y > 0.0f && fit.y > display.y)
            fit.y = display.y;
        ImGui::SetNextWindowSize(fit, ImGuiCond_Always);
    }
    if (ImGui::Begin("Options", &state.optionsOpen)) {
        // Content extent for the next fit, measured per item from the
        // window's top-left (so the title bar is included); every item counts
        // - the slider's width is fixed, so the measurement cannot feed back
        // into the fit.
        ImVec2 contentMax(0.0f, 0.0f);
        const ImVec2 winPos = ImGui::GetWindowPos();
        const auto trackExtent = [&contentMax, winPos] {
            const ImVec2 itemMax = ImGui::GetItemRectMax();
            if (const float x = itemMax.x - winPos.x; x > contentMax.x)
                contentMax.x = x;
            if (const float y = itemMax.y - winPos.y; y > contentMax.y)
                contentMax.y = y;
        };
        int percent =
            static_cast<int>(state.ui.prefs.fontScaleMain * PercentPerUnit + PercentRoundHalf);
        // Fixed width: the fit measures this item, so a window-relative
        // width would feed back into the next fit.
        ImGui::SetNextItemWidth(OptionsSliderWidthPx);
        ImGui::SliderInt("Text scale", &percent, MinFontScalePercent, MaxFontScalePercent, "%d%%");
        trackExtent();
        if (ImGui::IsItemEdited()) {
            percent =
                ((percent + model::kFontScaleStepPercent / 2) / model::kFontScaleStepPercent) *
                model::kFontScaleStepPercent;
            state.ui.prefs.fontScaleMain = percent / PercentPerUnit;
        }
        ImGui::Text("%d px", static_cast<int>(ImGui::GetFontSize()));
        trackExtent();
        if (const char* envFont = std::getenv("LLMFUN_GUI_FONT");
            envFont != nullptr && envFont[0] != '\0') {
            // FONT_SIZE only composes with FONT, hence the single-var hint.
            ImGui::TextDisabled("Base font from LLMFUN_GUI_FONT / _SIZE; px = base x scale");
            trackExtent();
        }
        state.optionsContentSize = contentMax;
    }
    ImGui::End();
}
} // namespace llmfun::tui
