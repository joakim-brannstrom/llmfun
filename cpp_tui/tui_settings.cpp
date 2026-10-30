/// GUI view settings persistence (imgui2.conf); see tui_settings.h.
#include "tui_settings.h"

// renderOptionsWindow (below) is ImGui-dependent and compiled out of the
// headless test build (TUI_SETTINGS_HEADLESS_TEST), which compiles this TU
// stdlib-only - so the ImGui/TuiState includes share that guard.
#ifndef TUI_SETTINGS_HEADLESS_TEST
#include "imgui.h"
#include "tui.h"
#endif

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>

namespace llmfun::tui {

namespace {

constexpr int MinWindowDimension = 1;
constexpr int MaxWindowDimension = 32767;
constexpr float MinFontScaleMain = 0.5f;
constexpr float MaxFontScaleMain = 3.0f;

// Text-scale UX constants: the load-acceptance range is Min/MaxFontScaleMain;
// the slider works in percent (percent = fontScaleMain * PercentPerUnit), with
// Min/MaxFontScalePercent as its range, FontScaleStepPercent as the quantize
// step, and PercentRoundHalf for the round-half percent readout.
constexpr int MinFontScalePercent = 50;
constexpr int MaxFontScalePercent = 300;
constexpr int FontScaleStepPercent = 5;
constexpr float PercentPerUnit = 100.0f;
constexpr float PercentRoundHalf = 0.5f;

// Fixed width of the Options window's text-scale slider: the content fit
// measures the slider item, so a window-dependent width would feed back into
// the next fit and make the window drift.
constexpr float OptionsSliderWidthPx = 200.0f;

// Compile-time tie between the percent constants and the scale constants: a
// silent divergence would let the slider save a value the load check then
// rejects at the next start (a reset to 1.0 with no diagnostic).
static_assert(MinFontScalePercent ==
              static_cast<int>(MinFontScaleMain * PercentPerUnit + PercentRoundHalf));
static_assert(MaxFontScalePercent ==
              static_cast<int>(MaxFontScaleMain * PercentPerUnit + PercentRoundHalf));

const char* skipTrailingWhitespace(char* end) {
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
        ++end;
    return end;
}

/// Strict integer parse (strtol): leading whitespace is skipped by strtol,
/// ERANGE or trailing content rejects.
bool parseStrictLong(const char* text, long& out) {
    errno = 0;
    char* end = nullptr;
    const long v = std::strtol(text, &end, 10);
    if (end == text || errno == ERANGE)
        return false;
    if (*skipTrailingWhitespace(end) != '\0')
        return false;
    out = v;
    return true;
}

/// Strict float parse (strtof): same rejection policy as parseStrictLong.
bool parseStrictFloat(const char* text, float& out) {
    errno = 0;
    char* end = nullptr;
    const float v = std::strtof(text, &end);
    if (end == text || errno == ERANGE)
        return false;
    if (*skipTrailingWhitespace(end) != '\0')
        return false;
    out = v;
    return true;
}

/// Clamp into [1, 32767]. Done by hand instead of std::clamp so the
/// stdlib-only include list stays at cstdio/cstdlib/cstring/string.
int clampWindowDimension(long v) {
    if (v < MinWindowDimension)
        return MinWindowDimension;
    if (v > MaxWindowDimension)
        return MaxWindowDimension;
    return static_cast<int>(v);
}

/// Apply one "Key=Value" line to the settings. Split at the first '=';
/// lines without '=' and unknown keys are skipped; malformed values keep
/// the current value. Keys are matched case-sensitively.
void applyLine(TuiSettings& settings, const std::string& line) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos)
        return;
    const std::string key = line.substr(0, eq);
    const std::string valueText = line.substr(eq + 1);

    if (key == "WindowWidth" || key == "WindowHeight") {
        long v = 0;
        if (!parseStrictLong(valueText.c_str(), v))
            return; // malformed keeps the current value
        const int clamped = clampWindowDimension(v);
        if (key == "WindowWidth")
            settings.windowWidth = clamped;
        else
            settings.windowHeight = clamped;
    } else if (key == "FontScaleMain") {
        float v = 0.0f;
        if (!parseStrictFloat(valueText.c_str(), v))
            return; // malformed keeps the current value
        // std::clamp passes NaN through, so the finite/range check is
        // explicit: NaN, inf, 1e40, 0 and out-of-range all keep the
        // current value.
        if (v > 0.0f && v >= MinFontScaleMain && v <= MaxFontScaleMain)
            settings.fontScaleMain = v;
    }
}

} // namespace

std::string settingsPath(const std::string& iniFilename) {
    if (iniFilename.empty())
        return std::string();
    const std::string suffix = ".ini";
    if (iniFilename.size() >= suffix.size() &&
        iniFilename.compare(iniFilename.size() - suffix.size(), suffix.size(), suffix) == 0)
        return iniFilename.substr(0, iniFilename.size() - suffix.size()) + ".conf";
    return iniFilename + ".conf";
}

void loadTuiSettings(TuiSettings& settings, const std::string& iniFilename) {
    if (iniFilename.empty())
        return;
    const std::string path = settingsPath(iniFilename);
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
        return; // missing file: the struct keeps its values
    std::string contents;
    std::array<char, 256> buf;
    size_t n = 0;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0)
        contents.append(buf.data(), n);
    std::fclose(f);

    size_t start = 0;
    while (start <= contents.size()) {
        size_t lineEnd = contents.find('\n', start);
        size_t stop = (lineEnd == std::string::npos) ? contents.size() : lineEnd;
        if (stop > start && contents[stop - 1] == '\r')
            --stop; // tolerate CRLF files
        applyLine(settings, contents.substr(start, stop - start));
        if (lineEnd == std::string::npos)
            break;
        start = lineEnd + 1;
    }
}

void saveTuiSettings(const TuiSettings& settings, const std::string& iniFilename) {
    if (iniFilename.empty())
        return;
    const std::string path = settingsPath(iniFilename);
    std::FILE* f = std::fopen(path.c_str(), "wt");
    if (f == nullptr)
        return; // best-effort: write failures are ignored silently
    // WindowWidth/WindowHeight are written only when > 0 so the struct's
    // 0 = "no persisted size" default never round-trips into a clamped 1.
    if (settings.windowWidth > 0)
        std::fprintf(f, "WindowWidth=%d\n", settings.windowWidth);
    if (settings.windowHeight > 0)
        std::fprintf(f, "WindowHeight=%d\n", settings.windowHeight);
    std::fprintf(f, "FontScaleMain=%.2f\n", settings.fontScaleMain);
    std::fclose(f);
}

#ifndef TUI_SETTINGS_HEADLESS_TEST
void renderOptionsWindow(TuiState& state) {
    if (!state.optionsOpen)
        return;
    // Fit the window to its content on the first open and on every text-scale
    // change: the extent is measured after every widget (the slider has a
    // fixed width, so the measurement is window-independent) and applied one
    // frame later with a 30 px right/bottom margin. Every other frame is left
    // alone, so user resizing sticks (and persists via imgui2.ini).
    constexpr float FitMarginPx = 30.0f;
    if (state.optionsFitScale != state.settings.fontScaleMain &&
        state.optionsContentSize.x > 0.0f && state.optionsContentSize.y > 0.0f) {
        state.optionsFitScale = state.settings.fontScaleMain;
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
            static_cast<int>(state.settings.fontScaleMain * PercentPerUnit + PercentRoundHalf);
        // Fixed width: the fit measures this item, so a window-relative
        // width would feed back into the next fit.
        ImGui::SetNextItemWidth(OptionsSliderWidthPx);
        ImGui::SliderInt("Text scale", &percent, MinFontScalePercent, MaxFontScalePercent, "%d%%");
        trackExtent();
        // Queried right after the slider (the last item; trackExtent submits
        // nothing): IsItemEdited gates
        // both the quantize and the write-back, so a read-only open never
        // normalizes a hand-edited FontScaleMain with extra decimals. A
        // per-frame write-back is unnecessary (the apply in `applyGuiFontScale()`
        // is idempotent). The "%d%%" label can transiently show an unquantized
        // value mid-drag (ImGui owns the slider value until release while the
        // font snaps in 5% steps) - intended, not a bug to "fix".
        if (ImGui::IsItemEdited()) {
            // SliderInt has no step parameter; quantize the edit to
            // FontScaleStepPercent instead.
            percent = ((percent + FontScaleStepPercent / 2) / FontScaleStepPercent) *
                      FontScaleStepPercent;
            state.settings.fontScaleMain = percent / PercentPerUnit;
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            // saveTuiSettings derives the .conf path itself - pass the raw
            // iniFilename; pre-deriving it here wrote imgui2.conf.conf.
            saveTuiSettings(state.settings, state.iniFilename);
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
#endif // TUI_SETTINGS_HEADLESS_TEST
} // namespace llmfun::tui
