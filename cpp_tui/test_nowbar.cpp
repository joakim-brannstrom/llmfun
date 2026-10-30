// test_nowbar.cpp
//
// The now bar (views/nowbar.h, design §4.7 / FR-11 / D-PHASE / D-FOLLOW, task 28): the
// seven phases, the four canonical chips, the degradation ladder and the `↓ latest` hint.
//
// What this binary proves, in order:
//
//   1. the source. `views/nowbar.cpp` writes no colour literal, takes the profile from the
//      metrics (never `tuiIsTextGrid()`), reads no display size, names no v4 type and does
//      not include `tui.h` - the layering every view is held to (`tools/gate_ratchet.txt`);
//   2. the vocabulary. Seven phases, seven distinct words, the ordinal round-trip, an
//      unknown ordinal clamped to idle, and the fourth chip's `Ready|Busy` mapping;
//   3. the chips. The structured status and the sentence splitter produce byte-identical
//      chips for the same numbers, and a number the status does not carry is `—`, never `0`;
//   4. the mapping. One case per phase: the word on the left, the mark beside it, the clock
//      only while a run is in flight;
//   5. the ladder. The hints go first, then the detail, then the chips collapse - and the
//      state is never cut, at 100, 60 and 40 cells;
//   6. the hints. `Esc stop` exactly while the Esc cancel step is open, `Ctrl+K commands`
//      always, and the "there is more below" mark in place of both while the thread is not
//      following (D-FOLLOW);
//   7. the badge. `RAW` from `UiState::rawLogOpen`, in compact mode - the one layout whose
//      top bar is folded away - and nowhere else;
//   8. the colours. The state rule clears 3:1 against the row's surface in every phase, and
//      every chip ink clears 4.5:1 against its own fill;
//   9. the screen. The row drawn through the real text backend: one row, nothing painted
//      outside it, the mark of every phase, and `-` where a number is missing;
//  10. the shell. `renderShell()` derives the row from the aggregate (the phase table, the
//      clock, the chips), projects the chips for the composer, and answers the compact
//      layout's collapsed chip form;
//  11. the goldens. Idle/running/offline at 100x30 and 60x20, each pinning the measured
//      parts and the painted fills as well as the glyphs.
//
// Exit codes: 0 = all checks pass; 1 = a check failed; 2 = usage error.
//
//   ./test_nowbar [-v] [--update] [--print]
//
// `--update` rewrites the fixtures; do that only for a deliberate rendering change, and
// commit the diff with it. They are captured with `LLMFUN_ASCII=0`, so a CJK locale or
// `TERM=linux` cannot change a golden.

#include "views/chrome.h"
#include "views/nowbar.h"
#include "views/shell.h"

#include "tui.h"
#include "tui_api.h"

#include "model/status_chips.h"
#include "ui/icons.h"
#include "ui/layout.h"
#include "ui/metrics.h"
#include "ui/text.h"
#include "ui/tokens.h"

#include "imtui/imtui.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

// `TuiState`: the ABI handle's private shape, shared with the other contract tests (one copy of
// the layout for all of them).
#include "test/tui_state_handle.h"

namespace ui = llmfun::tui::ui;
namespace views = llmfun::tui::views;
namespace model = llmfun::tui::model;
namespace tui = llmfun::tui;

namespace {

int g_checks = 0;
int g_failures = 0;
bool g_verbose = false;
bool g_print = false;

void check(bool condition, const std::string& what) {
    ++g_checks;
    if (condition) {
        if (g_verbose)
            std::printf("ok   %s\n", what.c_str());
        return;
    }
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++g_failures;
}

[[noreturn]] void usage(const char* argv0, const char* problem) {
    std::fprintf(stderr, "usage: %s [-v] [--update] [--print]   (%s)\n", argv0, problem);
    std::exit(2);
}

std::string num(float v) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.3f", double(v));
    std::string text = buffer;
    while (!text.empty() && text.back() == '0')
        text.pop_back();
    if (!text.empty() && text.back() == '.')
        text.pop_back();
    return text;
}

std::string quoted(const std::string& text) { return "'" + text + "'"; }

// ---------------------------------------------------------------------------
// The cell grid the text backend writes (the `TScreen` pattern)
// ---------------------------------------------------------------------------

ImTui::TScreen g_screen;

int g_w = 100;
int g_h = 30;

std::string utf8Of(uint32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return out;
}

struct Grid {
    int nx = 0;
    int ny = 0;
    std::vector<uint32_t> ch;
    std::vector<uint8_t> bg;

    uint32_t at(int x, int y) const {
        if (x < 0 || y < 0 || x >= nx || y >= ny)
            return 0;
        return ch[static_cast<std::size_t>(y) * nx + x];
    }
    uint8_t bgAt(int x, int y) const {
        if (x < 0 || y < 0 || x >= nx || y >= ny)
            return 0;
        return bg[static_cast<std::size_t>(y) * nx + x];
    }

    std::string rowText(int y) const {
        std::string text;
        for (int x = 0; x < nx; ++x) {
            const uint32_t cp = at(x, y);
            text += cp == 0 ? std::string(" ") : utf8Of(cp);
        }
        return text;
    }

    /// Every row's text, joined - the "the screen says this somewhere" search.
    std::string allText() const {
        std::string text;
        for (int y = 0; y < ny; ++y) {
            text += rowText(y);
            text += '\n';
        }
        return text;
    }

    bool hasText(const std::string& needle) const {
        return allText().find(needle) != std::string::npos;
    }
};

Grid capture() {
    Grid grid;
    grid.nx = g_screen.nx;
    grid.ny = g_screen.ny;
    const std::size_t cells = static_cast<std::size_t>(grid.nx) * grid.ny;
    grid.ch.resize(cells);
    grid.bg.resize(cells);
    for (std::size_t i = 0; i < cells; ++i) {
        grid.ch[i] = g_screen.data[i].ch;
        grid.bg[i] = g_screen.data[i].bg;
    }
    return grid;
}

/// One frame of the text grid at `g_w x g_h`: a decoration-free root window, the body
/// drawing inside it, then the frame through the real backend.
///
/// `inject` runs *before* `NewFrame`, because that is when ImGui consumes its input queue.
Grid frame(const std::function<void(const ui::Metrics&)>& body,
           const std::function<void(ImGuiIO&)>& inject = nullptr) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(g_w), static_cast<float>(g_h));
    io.DeltaTime = 1.0f / 60.0f;
    if (inject)
        inject(io);
    ImTui_ImplText_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(g_w), static_cast<float>(g_h)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("nowbar-frame", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus);
    body(ui::metricsFor(true, 1.0f, 1.0f));
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::Render();
    ImTui_ImplText_RenderDrawData(ImGui::GetDrawData(), &g_screen);
    if (g_print) {
        const Grid grid = capture();
        std::printf("--- frame %dx%d\n", g_w, g_h);
        for (int y = 0; y < grid.ny; ++y)
            std::printf("%2d |%s|\n", y, grid.rowText(y).c_str());
    }
    return capture();
}

// ---------------------------------------------------------------------------
// Fixtures and helpers
// ---------------------------------------------------------------------------

/// The row the layout engine hands the view at `w x h` - the geometry the frame really
/// answers for, not a rectangle this harness invented.
ui::Rect nowBarRect(int w, int h, const ui::Metrics& m) {
    ui::LayoutPrefs prefs;
    prefs.railOpen = true;
    prefs.inspectorOpen = true;
    const ui::ShellLayout layout =
        ui::computeShell(m, ImVec2(static_cast<float>(w), static_cast<float>(h)), prefs);
    return layout.nowBar;
}

/// The design's own numbers (§4.7's chip row), from the sentence - the split path.
model::StatusChips designChips() {
    return model::splitStatusText(
        "Context: 42000/128000 tokens | 38.1 tok/s | Model: 'qwen3-coder' | Ready");
}

views::NowBarInput idleRow() {
    views::NowBarInput in;
    in.phase = model::Phase::Idle;
    in.chips = designChips();
    in.following = true;
    return in;
}

views::NowBarInput runningRow(model::Phase phase) {
    views::NowBarInput in = idleRow();
    in.phase = phase;
    in.elapsedMs = 12000; // 00:12
    in.running = true;
    in.stopAvailable = true;
    in.chips.state = model::phaseStateWord(phase);
    return in;
}

/// The measured row for `in` at the layout's rectangle for `w x h`.
views::NowBarParts measureFor(const views::NowBarInput& in, int w, int h) {
    views::NowBarParts out;
    g_w = w;
    g_h = h;
    frame([&](const ui::Metrics& m) { out = views::nowBarMeasure(in, nowBarRect(w, h, m), m); });
    return out;
}

/// The grid the row paints at `w x h`.
Grid drawFor(const views::NowBarInput& in, int w, int h) {
    g_w = w;
    g_h = h;
    return frame([&](const ui::Metrics& m) { views::renderNowBar(in, nowBarRect(w, h, m), m); });
}

bool overlaps(const views::NowBarPart& a, const views::NowBarPart& b) {
    if (a.empty() || b.empty())
        return false;
    return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}

bool inside(const views::NowBarPart& part, const ui::Rect& box) {
    if (part.empty())
        return true;
    return part.x >= box.x - 0.01f && part.y >= box.y - 0.01f &&
           part.right() <= box.right() + 0.01f && part.bottom() <= box.bottom() + 0.01f;
}

std::string partText(const views::NowBarPart& part) {
    return num(part.x) + "," + num(part.y) + " " + num(part.w) + "x" + num(part.h);
}

float channel(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float luminance(ImVec4 c) {
    return 0.2126f * channel(c.x) + 0.7152f * channel(c.y) + 0.0722f * channel(c.z);
}

float contrastRatio(ImVec4 a, ImVec4 b) {
    const float la = luminance(a);
    const float lb = luminance(b);
    const float hi = std::max(la, lb);
    const float lo = std::min(la, lb);
    return (hi + 0.05f) / (lo + 0.05f);
}

std::string readWhole(const std::string& path) {
    std::ifstream in(path);
    if (!in.good())
        return std::string();
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------------------
// 1. The source obeys the layering rules
// ---------------------------------------------------------------------------

void checkSource() {
    const std::string source = readWhole(TEST_NOWBAR_SOURCE);
    check(!source.empty(), "views/nowbar.cpp is readable");
    check(source.find("tuiIsTextGrid") == std::string::npos,
          "the now bar takes the profile from the metrics, never from tuiIsTextGrid()");
    check(source.find("ImVec4(") == std::string::npos,
          "the now bar writes no colour literal (D-THEME)");
    check(source.find("DisplaySize") == std::string::npos,
          "the now bar reads no display size (a view is a geometry taker)");
    check(source.find("ChatMessage") == std::string::npos &&
              source.find("RenderGroup") == std::string::npos,
          "the now bar names no v4 type (D-ADAPTER)");
    check(source.find("#include \"tui.h\"") == std::string::npos,
          "the now bar does not include tui.h (views/ names no aggregate)");
    // The row must not reintroduce the v4 habit of formatting a number per frame: the
    // chips it draws are already formatted, and the only formatter it calls is the top
    // bar's clock.
    check(source.find("splitStatusText") == std::string::npos,
          "the row draws the chips it is given instead of splitting the sentence itself");
    check(source.find("formatCount(") == std::string::npos,
          "the row formats no count: the chips are canonical for the numbers (§6.3)");
    check(source.find("SetScrollHereY") == std::string::npos, "the row is not a scrolling surface");
}

// ---------------------------------------------------------------------------
// 2. The phase vocabulary (D-PHASE)
// ---------------------------------------------------------------------------

void checkVocabulary() {
    check(model::kPhaseCount == 7, "seven phases (design §4.7)");

    std::vector<std::string> words;
    std::vector<std::string> states;
    for (int i = 0; i < model::kPhaseCount; ++i) {
        const model::Phase phase = model::phaseFromOrdinal(i);
        check(model::phaseOrdinal(phase) == i,
              "ordinal " + std::to_string(i) + " round-trips through the phase table");
        const std::string word = model::phaseWord(phase);
        check(!word.empty(), "phase " + std::to_string(i) + " has a word");
        check(std::find(words.begin(), words.end(), word) == words.end(),
              "phase " + std::to_string(i) + " has a word of its own (" + word + ")");
        words.push_back(word);
        states.push_back(model::phaseStateWord(phase));
    }

    check(model::phaseWord(model::Phase::Idle) == std::string("Ready"),
          "idle reads Ready, the top bar's pill word");
    check(model::phaseWord(model::Phase::Thinking) == std::string("thinking"),
          "thinking is lower case, like the top bar's running pill");
    check(model::phaseWord(model::Phase::Offline) == std::string("offline"),
          "offline reads offline");

    // The fourth chip is the coarse state: Ready / Busy / the two the sentence cannot say.
    check(model::phaseStateWord(model::Phase::Idle) == std::string("Ready"),
          "the idle chip is Ready");
    for (const model::Phase phase : {model::Phase::Thinking, model::Phase::Tooling,
                                     model::Phase::Writing, model::Phase::Waiting}) {
        check(model::phaseStateWord(phase) == std::string("Busy"),
              "a working phase's chip is Busy (" + std::string(model::phaseWord(phase)) + ")");
    }
    check(model::phaseStateWord(model::Phase::Error) == std::string("Error"),
          "a failed run is not Busy");
    check(model::phaseStateWord(model::Phase::Offline) == std::string("Offline"),
          "an unreachable model is not Busy");

    // Unknown ordinals clamp instead of becoming a neighbour (task 39's rule).
    for (const int ordinal : {-1, 7, 99, -1000}) {
        check(model::phaseFromOrdinal(ordinal) == model::Phase::Idle,
              "ordinal " + std::to_string(ordinal) + " clamps to idle");
    }
}

// ---------------------------------------------------------------------------
// 3. The chips: one value, two paths (D-STATUS)
// ---------------------------------------------------------------------------

void checkChips() {
    // The structured status D will send (task 39), with the sentence's own numbers.
    model::RuntimeStatus status;
    status.phase = model::phaseOrdinal(model::Phase::Idle);
    status.contextUsed = 42000;
    status.contextMax = 128000;
    status.tokensPerSecond = 38.1;
    status.structured = true;

    const model::StatusChips structured = model::chipsFromStatus(status, "qwen3-coder");
    const model::StatusChips split = designChips();
    check(structured == split,
          "the structured and split paths produce byte-identical chips for the same numbers");
    check(structured.context == "42k/128k", "the structured context chip is 42k/128k");
    check(structured.speed == "38.1 tok/s", "the structured speed chip is 38.1 tok/s");
    check(structured.model == "qwen3-coder", "the structured model chip is the sentence's");
    check(structured.state == "Ready", "the structured state chip is the phase's");

    // A busy phase against the sentence D writes while a run is in flight.
    status.phase = model::phaseOrdinal(model::Phase::Thinking);
    const model::StatusChips busy = model::chipsFromStatus(status, "qwen3-coder");
    check(busy == model::splitStatusText(
                      "Context: 42000/128000 tokens | 38.1 tok/s | Model: 'qwen3-coder' | Busy"),
          "a thinking run and a `Busy` sentence produce the same chips");

    // A number the status does not carry is the absence value, never 0.
    model::RuntimeStatus empty;
    empty.structured = true;
    const model::StatusChips missing = model::chipsFromStatus(empty, "");
    check(missing.context == model::kMissingChipValue,
          "no capacity known renders the absence value, not 0/0");
    check(missing.speed == model::kMissingChipValue, "no rate known renders the absence value");
    check(missing.model == model::kMissingChipValue, "no model known renders the absence value");
    check(missing.contextUsed == 0 && missing.contextMax == 0,
          "and the meter's numbers stay 0 so nothing divides by a guess");
    check(missing.context.find('0') == std::string::npos, "no zero is drawn for a missing pair");

    // A used value of 0 with a real capacity is a *known* zero.
    model::RuntimeStatus fresh;
    fresh.structured = true;
    fresh.contextMax = 8192;
    const model::StatusChips freshChips = model::chipsFromStatus(fresh, "m");
    check(freshChips.context == "0/8.2k",
          "an empty context with a real capacity keeps both halves");

    // The chip set is a value: the frame layer compares instead of re-deriving.
    check(busy != split, "two different chip sets are not equal");
    check(!(busy == split), "and operator== agrees");
}

// ---------------------------------------------------------------------------
// 4. The mapping: one case per phase
// ---------------------------------------------------------------------------

void checkMapping() {
    struct Case {
        model::Phase phase;
        const char* word;
        const char* mark; // the glyph column (LLMFUN_ASCII=0)
    };
    const Case cases[model::kPhaseCount] = {
        {model::Phase::Idle, "Ready", ui::icon(ui::Icon::Ok, ui::GlyphSet::Glyphs)},
        {model::Phase::Thinking, "thinking", ui::icon(ui::Icon::Running, ui::GlyphSet::Glyphs)},
        {model::Phase::Tooling, "tooling", ui::icon(ui::Icon::Running, ui::GlyphSet::Glyphs)},
        {model::Phase::Writing, "writing", ui::icon(ui::Icon::Running, ui::GlyphSet::Glyphs)},
        {model::Phase::Waiting, "waiting", ui::icon(ui::Icon::Pending, ui::GlyphSet::Glyphs)},
        {model::Phase::Error, "error", ui::icon(ui::Icon::Error, ui::GlyphSet::Glyphs)},
        {model::Phase::Offline, "offline", ui::icon(ui::Icon::Warn, ui::GlyphSet::Glyphs)},
    };

    for (const Case& item : cases) {
        const std::string where = std::string("phase ") + item.word;

        // The measured row.
        views::NowBarInput in =
            item.phase == model::Phase::Idle ? idleRow() : runningRow(item.phase);
        const views::NowBarParts parts = measureFor(in, 100, 30);
        check(parts.stateText == std::string(item.word), where + ": the row says the word");
        check(parts.mark.w > 0.0f && parts.mark.h > 0.0f, where + ": the mark has a slot");
        check(!parts.state.empty(), where + ": the word has room");
        check(parts.mark.right() <= parts.state.x + 0.01f,
              where + ": the mark is left of the word, never over it");

        // The screen.
        const Grid grid = drawFor(in, 100, 30);
        check(grid.hasText(std::string(item.mark) + " " + item.word),
              where + ": the screen shows the mark beside the word");
        check(grid.hasText("ctx 42k/128k"), where + ": the context chip is on the row");
        check(grid.hasText("38.1 tok/s"), where + ": the speed chip is on the row");
        check(grid.hasText("qwen3-coder"), where + ": the model chip is on the row");
        check(grid.hasText(model::phaseStateWord(item.phase)),
              where + ": the state chip is on the row");
    }
}

void checkClock() {
    // Idle has no clock: a time beside `Ready` would claim a run that is not happening
    // (the top bar's pill rule, applied to this row).
    const views::NowBarParts idle = measureFor(idleRow(), 100, 30);
    check(idle.tailText.find("00:") == std::string::npos, "an idle row draws no clock");

    // A run draws it, even on its first second.
    views::NowBarInput justStarted = runningRow(model::Phase::Thinking);
    justStarted.elapsedMs = 0;
    const views::NowBarParts started = measureFor(justStarted, 100, 30);
    check(started.tailText.find("00:00") != std::string::npos,
          "a run's first frame still shows the clock (00:00)");

    views::NowBarInput twelveSeconds = runningRow(model::Phase::Thinking);
    twelveSeconds.elapsedMs = 12000;
    const views::NowBarParts later = measureFor(twelveSeconds, 100, 30);
    check(later.tailText.find("00:12") != std::string::npos, "the run clock reads 00:12");

    // The detail joins the tail, ahead of the clock.
    views::NowBarInput withDetail = twelveSeconds;
    withDetail.detail = "executeCommand";
    const views::NowBarParts detailed = measureFor(withDetail, 100, 30);
    check(detailed.tailText.find("executeCommand") != std::string::npos,
          "the current step is drawn");
    check(detailed.tailText.find("executeCommand") < detailed.tailText.find("00:12"),
          "the step comes before the clock");
}

// ---------------------------------------------------------------------------
// 5. The ladder: hints, then the detail, then the chips - never the state
// ---------------------------------------------------------------------------

void checkLadder() {
    const views::NowBarInput in = runningRow(model::Phase::Writing);

    // 100 cells: everything, hints included.
    const views::NowBarParts wide = measureFor(in, 100, 30);
    check(!wide.hintsText.empty(), "100 cells: the hints are drawn");
    check(wide.hintsText.find("Esc stop") != std::string::npos,
          "100 cells: the stop hint is drawn while a run is in flight");
    check(wide.hintsText.find("Ctrl+K") != std::string::npos,
          "100 cells: the palette hint is drawn");
    check(!wide.compactChips, "100 cells: the four chips are the full form");
    check(!wide.chipCtx.empty() && !wide.chipSpeed.empty() && !wide.chipModel.empty() &&
              !wide.chipState.empty(),
          "100 cells: all four chips have a slot");

    // 60 cells: the state is never cut, and the hints are what gives way when they must.
    // (At this edge the four chips no longer fit beside the state even with no hints and
    // no clock, so they take their compact form here - "below 60" is where that becomes
    // necessary, not where the design first allows it.)
    const views::NowBarParts narrow = measureFor(in, 60, 20);
    check(narrow.stateText == std::string(model::phaseWord(in.phase)),
          "60 cells: the state word is not cut");
    check(!narrow.chipCtx.empty() || !narrow.chipState.empty(),
          "60 cells: the numbers keep a chip");

    // 80 cells: the four chips fit, so the hints are the ones that give way - never the
    // state (design §4.7's degradation order). A hint is dropped whole: no ellipsis, no
    // dangling separator, no half a word.
    const views::NowBarParts medium = measureFor(in, 80, 24);
    check(!medium.compactChips, "80 cells: the four chips are still the full form");
    check(medium.stateText == std::string(model::phaseWord(in.phase)),
          "80 cells: the state word is not cut");
    check(medium.hintsTruncated, "80 cells: the hint list is not drawn in full");
    check(medium.hintsText == std::string(views::kNowBarHintStop),
          "80 cells: only the whole, most contextual hint is drawn (" + medium.hintsText + ")");
    check(medium.hintsText.find(ui::icon(ui::Icon::Ellipsis, ui::GlyphSet::Glyphs)) ==
              std::string::npos,
          "80 cells: and it is not cut (no ellipsis in a hint list)");

    // 40 cells: the floor. The state survives, the chips collapse.
    const views::NowBarParts floorRow = measureFor(in, 40, 15);
    check(floorRow.stateText == std::string(model::phaseWord(in.phase)),
          "40 cells: the state word still is whole");
    check(floorRow.compactChips, "40 cells: the chips are the collapsed form");
    check(!floorRow.chipCtx.empty(), "40 cells: the ctx chip survives");
    check(floorRow.chipStateText.empty() && !floorRow.chipState.empty(),
          "40 cells: the state is a mark, not a word, in the collapsed chip");

    // Nothing overlaps, and nothing leaves the row, at every supported size.
    for (const int size : {120, 100, 80, 60, 40}) {
        const int height = size >= 80 ? 30 : 20;
        g_w = size;
        g_h = height;
        const views::NowBarParts parts = measureFor(in, size, height);
        frame([&](const ui::Metrics& m) {
            const ui::Rect bar = nowBarRect(size, height, m);
            check(parts.rule.x >= bar.x - 0.01f && parts.state.right() <= bar.right() + 0.01f,
                  "row " + std::to_string(size) + ": the state stays inside the row");
            const views::NowBarPart all[] = {
                parts.rule,  parts.mark,    parts.state,     parts.tail,      parts.hints,
                parts.badge, parts.chipCtx, parts.chipSpeed, parts.chipModel, parts.chipState};
            bool allInside = true;
            for (const views::NowBarPart& part : all)
                allInside = allInside && inside(part, bar);
            check(allInside, "row " + std::to_string(size) + ": every part is inside the row");
            bool noOverlap = true;
            for (std::size_t a = 0; a < std::size(all); ++a)
                for (std::size_t b = a + 1; b < std::size(all); ++b)
                    noOverlap = noOverlap && !overlaps(all[a], all[b]);
            check(noOverlap, "row " + std::to_string(size) + ": no two parts overlap");
        });
    }

    // A running row that cannot fit its detail drops the detail before the clock.
    views::NowBarInput detailed = runningRow(model::Phase::Tooling);
    detailed.detail = "a_considerably_longer_tool_name_than_any_row_can_hold";
    const views::NowBarParts squeezed = measureFor(detailed, 60, 20);
    check(squeezed.tailText.find("00:12") != std::string::npos,
          "a narrow row keeps the clock when the tool name does not fit");
    check(squeezed.tailText.find("a_considerably") == std::string::npos,
          "and drops the tool name instead of cutting the clock");
}

// ---------------------------------------------------------------------------
// 6. The hints (D-ESC's stop step, D-FOLLOW's "more below")
// ---------------------------------------------------------------------------

void checkHints() {
    // `Esc stop` is drawn exactly while the Esc cancel step is open.
    views::NowBarInput running = runningRow(model::Phase::Thinking);
    const views::NowBarParts stopOpen = measureFor(running, 100, 30);
    check(stopOpen.hintsText.find(views::kNowBarHintStop) != std::string::npos,
          "`Esc stop` is drawn while the cancel step is open");

    views::NowBarInput stopping = running;
    stopping.stopAvailable = false; // a stop is already on its way
    stopping.phase = model::Phase::Waiting;
    const views::NowBarParts stopClosed = measureFor(stopping, 100, 30);
    check(stopClosed.hintsText.find(views::kNowBarHintStop) == std::string::npos,
          "no `Esc stop` once the stop has been requested (the press would act twice)");
    check(stopClosed.hintsText.find("Ctrl+K") != std::string::npos, "the palette hint stays");

    const views::NowBarParts idle = measureFor(idleRow(), 100, 30);
    check(idle.hintsText.find(views::kNowBarHintStop) == std::string::npos,
          "an idle row advertises no stop");
    check(idle.hintsText == std::string(views::kNowBarHintPalette),
          "an idle row's only hint is the palette");

    // Not following the newest content: the hints make way for the one thing the row has
    // to say then (design §5.11, D-FOLLOW).
    views::NowBarInput reading = runningRow(model::Phase::Writing);
    reading.following = false;
    const views::NowBarParts notFollowing = measureFor(reading, 100, 30);
    check(notFollowing.hintsText.find("latest") != std::string::npos,
          "a row that is not following says there is more below");
    check(notFollowing.hintsText.find("Ctrl+K") == std::string::npos,
          "and replaces the contextual hints instead of joining them");
    check(notFollowing.hintsText.rfind(ui::icon(ui::Icon::DisclosureOpen, ui::GlyphSet::Glyphs),
                                       0) == 0,
          "the hint starts with the table's down mark (the design's `↓`, from ui/icons.h)");

    reading.following = true;
    const views::NowBarParts backToFollowing = measureFor(reading, 100, 30);
    check(backToFollowing.hintsText.find("latest") == std::string::npos,
          "the mark disappears the moment the thread follows again");

    // An open question owns the hint slot, and outranks both of the above. It is never a
    // fragment: the prompt's tail is the instruction, so a narrow row says its short form
    // (and, below that, takes the metric chips' room) rather than "run active - press Ctrl+…".
    views::NowBarInput asked = runningRow(model::Phase::Thinking);
    asked.prompt = tui::kQuitConfirmHint;
    asked.promptShort = tui::kQuitConfirmShort;
    const views::NowBarParts prompt = measureFor(asked, 100, 30);
    check(prompt.hintsText == std::string(tui::kQuitConfirmHint),
          "a question replaces the hints (the whole prompt fits at 100 cells)");
    check(prompt.hintsText.find("Ctrl+Q") != std::string::npos,
          "and names the key that answers it");
    check(prompt.stateText == std::string("thinking"), "the state is still on the row");
    check(prompt.tailText.empty(), "the step and the clock step aside for the question");
    check(!prompt.chipState.empty(), "and the state chip keeps its slot");

    // The size ladder: full prompt, then the short form, then the chips' room - the key is
    // named at every size, including the plan's 40x15 floor.
    for (const int size : {120, 100, 80, 60, 50, 40}) {
        const int height = size >= 80 ? 30 : (size >= 60 ? 20 : 15);
        const views::NowBarParts row = measureFor(asked, size, height);
        check(row.hintsText.find("Ctrl+Q") != std::string::npos,
              "the question still names its key at " + std::to_string(size) + " cells (" +
                  row.hintsText + ")");
        check(row.stateText == std::string("thinking"),
              "and the state word survives at " + std::to_string(size) + " cells");
        check(row.hintsText.find(ui::icon(ui::Icon::Ellipsis, ui::GlyphSet::Glyphs)) ==
                  std::string::npos,
              "and no question is cut at " + std::to_string(size) + " cells");
        check(row.hints.x >= row.state.right() - 0.01f,
              "and the question never runs into the state at " + std::to_string(size) + " cells");
    }

    // At 60 the question is the short form; at 40 it has taken the metric chips' room.
    const views::NowBarParts sixty = measureFor(asked, 60, 20);
    check(sixty.hintsText == std::string(tui::kQuitConfirmShort),
          "60 cells: the question takes its short form (" + sixty.hintsText + ")");
    check(!sixty.chipCtx.empty(), "60 cells: the ctx chip is still there");
    const views::NowBarParts forty = measureFor(asked, 40, 15);
    check(forty.chipCtx.empty() && forty.chipState.empty(),
          "40 cells: the metric chips step aside for the question");
    check(forty.hintsText == std::string(tui::kQuitConfirmShort),
          "40 cells: and the short form fits whole");

    asked.following = false;
    const views::NowBarParts promptAndReading = measureFor(asked, 100, 30);
    check(promptAndReading.hintsText == std::string(tui::kQuitConfirmHint),
          "a question outranks the `more below` mark too");
}

// ---------------------------------------------------------------------------
// 7. The `RAW` badge (D-RAWLOG)
// ---------------------------------------------------------------------------

void checkBadge() {
    views::NowBarInput compactOpen = idleRow();
    compactOpen.compact = true;
    compactOpen.rawLogOpen = true;
    const Grid badge = drawFor(compactOpen, 40, 15);
    check(badge.hasText(views::kRawBadge),
          "compact: the RAW badge is on the now bar (the top bar is folded away)");
    check(measureFor(compactOpen, 40, 15).compactChips,
          "compact: the chips are the collapsed form");

    views::NowBarInput compactClosed = compactOpen;
    compactClosed.rawLogOpen = false;
    check(!drawFor(compactClosed, 40, 15).hasText(views::kRawBadge),
          "compact and closed: no badge");

    views::NowBarInput wideOpen = idleRow();
    wideOpen.rawLogOpen = true;
    check(!drawFor(wideOpen, 100, 30).hasText(views::kRawBadge),
          "the full tiling leaves the badge to the top bar, where it already is");

    // Focus mode (task 43) hides the top bar without collapsing anything else, so the badge moves
    // here while the four chips stay whole: hiding a region may not hide a safety signal.
    views::NowBarInput focusOpen = idleRow();
    focusOpen.topBarFolded = true;
    focusOpen.rawLogOpen = true;
    check(drawFor(focusOpen, 100, 30).hasText(views::kRawBadge),
          "focus mode: the badge moves to the now bar with the top bar hidden");
    check(!measureFor(focusOpen, 100, 30).compactChips,
          "focus mode: ...and the chips keep their full form (unlike compact mode)");
    views::NowBarInput focusClosed = focusOpen;
    focusClosed.rawLogOpen = false;
    check(!drawFor(focusClosed, 100, 30).hasText(views::kRawBadge),
          "focus mode and a closed Raw Log: no badge");
}

// ---------------------------------------------------------------------------
// 7b. The `Esc leave focus` hint (task 43)
// ---------------------------------------------------------------------------

/// With the mode on, the next `Esc` leaves it - so that is what the row must say. It is the one
/// hint that survives the mode, because the terminal's way *in* (the palette's row) is not on
/// screen once the mode is up. The stop hint cannot share the row with it: the derivation asks the
/// resolver (`model::escapeStopsRun`), so a run in flight shows the focus hint, not `Esc stop`.
void checkFocusHint() {
    views::NowBarInput idle = idleRow();
    idle.focusMode = true;
    const views::NowBarParts idleParts = measureFor(idle, 120, 30);
    check(idleParts.hintsText.find(views::kNowBarHintLeaveFocus) != std::string::npos,
          "focus hint: the row names the mode's exit (\"" + idleParts.hintsText + "\")");
    check(drawFor(idle, 120, 30).hasText(views::kNowBarHintLeaveFocus),
          "focus hint: ...and it is drawn");
    check(idleParts.hintsText.find(views::kNowBarHintPalette) != std::string::npos,
          "focus hint: the palette hint still follows it");

    // The mode plus a run: the mode's exit wins the slot, because that is the press that happens.
    views::NowBarInput busy = idleRow();
    busy.focusMode = true;
    busy.running = true;
    busy.stopAvailable = false; // what `deriveNowBar()` answers in the mode (`escapeStopsRun`)
    const views::NowBarParts busyParts = measureFor(busy, 120, 30);
    check(busyParts.hintsText.find(views::kNowBarHintLeaveFocus) != std::string::npos &&
              busyParts.hintsText.find(views::kNowBarHintStop) == std::string::npos,
          "focus hint: a run in the mode still advertises the mode's exit, not Esc stop (" +
              busyParts.hintsText + ")");

    // Without the mode the row is what it always was.
    views::NowBarInput plain = idleRow();
    check(measureFor(plain, 120, 30).hintsText.find(views::kNowBarHintLeaveFocus) ==
              std::string::npos,
          "focus hint: the ordinary row does not mention the mode");
}

// ---------------------------------------------------------------------------
// 8. The colours
// ---------------------------------------------------------------------------

void checkColours() {
    // The state rule is a non-text boundary: 3:1 against the row's own fill.
    const ImVec4 surface = ui::color(ui::ColorToken::surface);
    const ImVec4 raised = ui::color(ui::ColorToken::raised);
    struct Ink {
        model::Phase phase;
        const char* what;
    };
    const Ink inks[model::kPhaseCount] = {
        {model::Phase::Idle, "idle"},       {model::Phase::Thinking, "thinking"},
        {model::Phase::Tooling, "tooling"}, {model::Phase::Writing, "writing"},
        {model::Phase::Waiting, "waiting"}, {model::Phase::Error, "error"},
        {model::Phase::Offline, "offline"},
    };
    for (const Ink& ink : inks) {
        // The rule's ink is the mark's ink, read back through the same table the view
        // uses (`lookFor` is private, so this asks the drawn screen instead: the rule is
        // the leftmost cell, and its background is the phase's colour).
        views::NowBarInput in = ink.phase == model::Phase::Idle ? idleRow() : runningRow(ink.phase);
        const Grid grid = drawFor(in, 100, 30);
        // The row is the last line of the display.
        const int row = grid.ny - 1;
        check(grid.bgAt(0, row) != 0, std::string("the ") + ink.what + " state rule is painted");
    }

    // Every chip ink clears the 4.5:1 text bar against the chip fill it is drawn on.
    const ImVec4 metricInk = ui::color(ui::ColorToken::textMid);
    check(contrastRatio(metricInk, raised) >= 4.5f,
          "the metric chips' ink clears 4.5:1 on their fill");
    for (const Ink& ink : inks) {
        const ImVec4 markInk =
            ui::color(ink.phase == model::Phase::Idle      ? ui::ColorToken::textMid
                      : ink.phase == model::Phase::Waiting ? ui::ColorToken::stateWarn
                      : ink.phase == model::Phase::Error || ink.phase == model::Phase::Offline
                          ? ui::ColorToken::stateDanger
                          : ui::ColorToken::accent);
        check(contrastRatio(markInk, raised) >= 4.5f,
              std::string("the ") + ink.what + " state chip's ink clears 4.5:1");
        check(contrastRatio(markInk, surface) >= 4.5f,
              std::string("and the ") + ink.what + " word clears 4.5:1 on the row itself");
    }
    check(contrastRatio(ui::color(ui::ColorToken::textLow), surface) >= 4.5f,
          "the hints' dim ink still clears 4.5:1 on the row");
}

// ---------------------------------------------------------------------------
// 9. The screen: one row, nothing outside it, missing values as `-`
// ---------------------------------------------------------------------------

void checkScreen() {
    views::NowBarInput in = runningRow(model::Phase::Thinking);
    in.detail = "read_file";
    const Grid grid = drawFor(in, 100, 30);

    // One row: the row's own line carries text, every other line is blank.
    int paintedRows = 0;
    for (int y = 0; y < grid.ny; ++y) {
        const std::string row = grid.rowText(y);
        if (row.find_first_not_of(' ') != std::string::npos)
            ++paintedRows;
    }
    check(paintedRows == 1,
          "the row paints exactly one line (" + std::to_string(paintedRows) + ")");

    const int row = grid.ny - 1;
    const std::string text = grid.rowText(row);
    check(text.find("thinking") != std::string::npos, "the phase word is on the last row");
    check(text.find("read_file") != std::string::npos, "the current step is on the row");
    check(text.find("00:12") != std::string::npos, "the clock is on the row");
    check(text.find("Esc stop") != std::string::npos, "the stop hint is on the row");

    // A missing number: `-` in the grid (U+2014 is East-Asian ambiguous), never `0`.
    views::NowBarInput missing = idleRow();
    missing.chips = model::splitStatusText("Ready"); // the state, no numbers
    const Grid missingGrid = drawFor(missing, 100, 30);
    const std::string missingText = missingGrid.rowText(missingGrid.ny - 1);
    check(missingText.find("ctx -") != std::string::npos,
          "a missing context renders the grid's absence glyph beside `ctx`");
    check(missingText.find("0") == std::string::npos, "and never a 0");
    check(!missingText.empty() && missingGrid.hasText("Ready"), "the state word is still drawn");

    // The text grid never draws the model's em dash (it may be two cells in a CJK
    // terminal, which would shift every chip after it).
    check(missingText.find("\u2014") == std::string::npos,
          "the grid uses the ASCII floor for an absent value");

    // A degenerate rectangle paints nothing.
    g_w = 100;
    g_h = 30;
    const Grid empty = frame([&](const ui::Metrics& m) {
        views::renderNowBar(idleRow(), ui::Rect{0.0f, 0.0f, 0.0f, 0.0f, m.textGrid}, m);
    });
    check(empty.rowText(empty.ny - 1).find_first_not_of(' ') == std::string::npos,
          "a zero rectangle paints nothing");
}

// ---------------------------------------------------------------------------
// 10. The shell: the derivation, the composer's phase word, compact mode
// ---------------------------------------------------------------------------

views::ShellFrame shellFrame(tui::TuiState& state, int w, int h) {
    views::ShellFrame out;
    g_w = w;
    g_h = h;
    frame([&](const ui::Metrics&) {
        out = views::renderShell(state, ImVec2(static_cast<float>(w), static_cast<float>(h)));
    });
    return out;
}

void checkShell() {
    tui::TuiState state;
    // The sentence D sends, through the API D uses (a direct write would also skip the
    // setter's "unchanged sentence" rule, which is what keeps the chips off the frame path).
    tui::tuiSetStatusText(
        state, "Context: 42000/128000 tokens | 38.1 tok/s | Model: 'qwen3-coder' | Ready");

    // Idle frame: the phase is idle and the chips are the sentence's.
    shellFrame(state, 100, 30);
    check(state.ui.status.phase == model::phaseOrdinal(model::Phase::Idle),
          "an idle frame derives the idle phase");
    check(state.ui.chips == designChips(), "and projects the sentence's chips");
    check(!state.ui.status.structured,
          "a state that never received a structured status keeps the sentence path");

    // A run: the phase follows the facts the frame owns.
    state.readyStatus = false;
    shellFrame(state, 100, 30);
    check(state.ui.status.phase == model::phaseOrdinal(model::Phase::Thinking),
          "a run with no answer text yet is `thinking`");
    check(state.ui.status.elapsedMs >= 0, "the clock is derived with it");

    state.chat.hasStreamMsg = true;
    state.chat.streamMsg.text = "partial answer";
    shellFrame(state, 100, 30);
    check(state.ui.status.phase == model::phaseOrdinal(model::Phase::Writing),
          "a run whose answer is arriving is `writing`");

    state.cancelRequested = true;
    shellFrame(state, 100, 30);
    check(state.ui.status.phase == model::phaseOrdinal(model::Phase::Waiting),
          "a requested stop is `waiting`");

    state.chat.hasStreamMsg = false;
    state.chat.streamMsg.text.clear();
    state.ui.offline = true;
    state.readyStatus = true;
    shellFrame(state, 100, 30);
    check(state.ui.status.phase == model::phaseOrdinal(model::Phase::Offline),
          "an unreachable model is `offline`");
    check(state.ui.chips.state == std::string("Offline"),
          "and its state chip says Offline, not the sentence's Ready");

    // The composer's phase line reads the same word as the row (one vocabulary).
    state.ui.offline = false;
    state.readyStatus = false;
    state.cancelRequested = false;
    g_w = 100;
    g_h = 30;
    const Grid grid = frame([&](const ui::Metrics&) {
        views::renderShell(state, ImVec2(static_cast<float>(g_w), static_cast<float>(g_h)));
    });
    check(grid.hasText("thinking"), "the composer's meta row says `thinking`, not `Busy`");
    check(grid.hasText("38.1 tok/s"), "and the same speed chip as the now bar");

    // Compact mode: the layout's answer, and the collapsed chip form with it.
    state.readyStatus = true;
    const views::ShellFrame compact = shellFrame(state, 50, 20);
    check(compact.layout.compact, "a 50-cell display is compact");
    check(compact.layout.topBar.h == 0.0f, "compact mode folds the top bar away");
    check(compact.layout.nowBar.h > 0.0f, "and keeps the now bar's row");

    // A tiny display keeps the thread's three rows and the row's one.
    check(compact.layout.nowBar.h == ui::metricsFor(true, 1.0f, 1.0f).nowBarH,
          "the now bar is exactly one row tall");

    // The `RAW` badge in compact mode comes from the aggregate's own flag.
    //
    // The row is drawn directly here rather than through `renderShell()`: an *open* Raw Log is
    // the text grid's full-screen overlay (task 33), so a shell frame with the surface up paints
    // over the now bar - that is what "full-screen" means, and `./test_rawlog` pins it. What this
    // check is about is the row's half: the flag reaches the badge.
    views::NowBarInput badgeIn = views::NowBarInput{};
    badgeIn.compact = true;
    badgeIn.rawLogOpen = true;
    const Grid compactRaw = drawFor(badgeIn, 50, 20);
    check(compactRaw.hasText(views::kRawBadge),
          "an open Raw Log puts the RAW badge in the compact now bar");
    badgeIn.rawLogOpen = false;
    check(!drawFor(badgeIn, 50, 20).hasText(views::kRawBadge), "and a closed one does not");
}

void checkStopHint() {
    // "Stop is present and functional while running": the hint is drawn exactly while the
    // Esc cancel step is open, and the press it advertises is the frame's (`tui.cpp`'s
    // dispatch turns it into the shipped `tuiRequestCancel`, and `./test_keymap` S6 pins
    // that path). What is pinned here is the row's half of it - that the hint tracks the
    // step's availability end to end, from the aggregate through `cancelStepOpen()`.
    tui::TuiState state;
    tui::tuiSetStatusText(
        state, "Context: 42000/128000 tokens | 38.1 tok/s | Model: 'qwen3-coder' | Busy");
    state.readyStatus = false;
    shellFrame(state, 100, 30);
    check(tui::cancelStepOpen(state), "a running, not-yet-stopped frame has the Esc step open");
    check(capture().hasText(views::kNowBarHintStop), "and the row advertises it");

    state.cancelRequested = true; // a stop is on its way
    shellFrame(state, 100, 30);
    check(!tui::cancelStepOpen(state), "a requested stop closes the Esc step");
    check(!capture().hasText(views::kNowBarHintStop),
          "so the hint is gone (the press would act on the input instead)");
}

/// The same rule, one step up the chain: focus mode owns the press before the cancel step does, so
/// a run in the mode advertises *the mode's exit*, not `Esc stop` (task 43, the review's I1). This
/// is the derivation end to end - `deriveNowBar()` asks `model::escapeStopsRun`, and the assertion
/// is on the drawn row, so a hint that read the run's state directly fails here.
void checkFocusStopHint() {
    tui::TuiState state;
    tui::tuiSetStatusText(
        state, "Context: 42000/128000 tokens | 38.1 tok/s | Model: 'qwen3-coder' | Busy");
    state.readyStatus = false;
    shellFrame(state, 100, 30);
    check(capture().hasText(views::kNowBarHintStop),
          "premise: a running frame advertises the stop");

    state.ui.focusMode = true;
    const views::ShellFrame frame = shellFrame(state, 140, 30);
    check(!frame.nowBar.stopAvailable,
          "focus mode: the row's stop flag is off (the press would leave the mode)");
    check(!capture().hasText(views::kNowBarHintStop),
          "focus mode: ...so the row stops promising `Esc stop`");
    check(capture().hasText(views::kNowBarHintLeaveFocus),
          "focus mode: it names the press it will perform instead");
    // The row keeps its ladder: the mode's hint is the *first* part, so the palette hint follows it
    // wherever both fit (140 cells here, so both do).
    check(capture().hasText(views::kNowBarHintPalette),
          "focus mode: and the palette hint still follows it");

    state.ui.focusMode = false;
    shellFrame(state, 100, 30);
    check(capture().hasText(views::kNowBarHintStop),
          "leaving the mode puts the stop hint back (the next press cancels)");
}

void checkOverlayHint() {
    // A hint must not outlive the step it advertises: an overlay owns the *first* `Esc`
    // step (`resolveEscape({overlay, cancel, …})`), so while one is up the row stops
    // promising a stop - `Ctrl+K` over a running agent is the everyday case.
    tui::TuiState state;
    tui::tuiSetStatusText(
        state, "Context: 42000/128000 tokens | 38.1 tok/s | Model: 'qwen3-coder' | Busy");
    state.readyStatus = false;
    shellFrame(state, 100, 30);
    check(capture().hasText(views::kNowBarHintStop), "a running frame advertises the stop");

    state.palette.open = true;
    shellFrame(state, 100, 30);
    check(state.overlayOwnsKeyboard(), "the palette owns the keyboard");
    check(!capture().hasText(views::kNowBarHintStop),
          "so the row stops promising `Esc stop` (Esc closes the palette first)");
    state.palette.open = false;

    shellFrame(state, 100, 30);
    check(capture().hasText(views::kNowBarHintStop), "and the hint returns when it closes");
}

void checkQuitPrompt() {
    // Task 28's note: the v4 status row has to hide the four chips to show the quit
    // confirmation, so the run state disappears from the screen. This row has room for
    // both - and it keeps the *question* whole, which is the half the v4 row gets right
    // and a naive truncation would not: the prompt's tail is the instruction.
    tui::TuiState state;
    tui::tuiSetStatusText(
        state, "Context: 42000/128000 tokens | 38.1 tok/s | Model: 'qwen3-coder' | Busy");
    state.readyStatus = false;
    state.quitConfirmArmed = true; // what the Ctrl+Q chord sets while a run is active

    shellFrame(state, 100, 30);
    const Grid grid = capture();
    check(grid.hasText("Ctrl+Q"), "an armed quit confirmation is visible in the now bar");
    check(grid.hasText("run active - press"), "100 cells: the whole sentence fits");
    check(grid.hasText("thinking"), "the phase word survives it");
    // The chips compress to their key form rather than the question being cut: the state
    // keeps its mark at the row's right end (measured in `checkHints`) and the one number
    // everybody reads stays.
    check(grid.hasText("ctx 42k"), "and the numbers survive in their compact form");
    check(!grid.hasText(views::kNowBarHintStop), "the prompt takes the hint slot");

    // The size ladder, end to end: every size names the key. A fragment ("run acti…") is
    // the one thing the row must not show, because it is the row's only control that has to
    // be answered.
    for (const auto& size : {std::pair<int, int>{80, 30}, {60, 20}, {50, 15}, {40, 15}}) {
        g_w = size.first;
        g_h = size.second;
        shellFrame(state, size.first, size.second);
        const Grid row = capture();
        check(row.hasText("Ctrl+Q"),
              "the confirmation still names its key at " + std::to_string(size.first) + " cells");
        check(row.hasText("thinking"),
              "and the state word is still drawn at " + std::to_string(size.first) + " cells");
    }

    state.quitConfirmArmed = false;
    shellFrame(state, 100, 30);
    check(capture().hasText(views::kNowBarHintStop), "and the hints come back once it is disarmed");
}

// ---------------------------------------------------------------------------
// 11. The goldens
// ---------------------------------------------------------------------------

struct Scene {
    const char* name;
    model::Phase phase;
    bool running;
    bool offline;
    bool rawLogOpen;
    int w, h;
    /// The feedback item the scene raises before the shell draws (task 45): nullptr for the
    /// ordinary scenes, so their goldens pin the row without a flash.
    const char* flash = nullptr;
    model::ToastKind flashKind = model::ToastKind::Info;
};

std::vector<Scene> scenes() {
    return {
        {"nowbar-idle", model::Phase::Idle, false, false, false, 100, 30},
        {"nowbar-running", model::Phase::Writing, true, false, false, 100, 30},
        {"nowbar-offline", model::Phase::Offline, false, true, false, 100, 30},
        {"nowbar-idle", model::Phase::Idle, false, false, false, 60, 20},
        {"nowbar-running", model::Phase::Writing, true, false, false, 60, 20},
        {"nowbar-offline", model::Phase::Offline, false, true, false, 60, 20},
        // Compact (below 60 cells): the top bar is folded away, so the chips collapse and
        // the `RAW` badge is this row's - the one place the design puts it there.
        {"nowbar-compact", model::Phase::Writing, true, false, true, 50, 20},
        // The flash (task 45): a transient answer at 100 cells and a persistent one at 60, where
        // the row can no longer afford the key that clears it and draws the message alone.
        {"nowbar-flash", model::Phase::Idle, false, false, false, 100, 30, "Copied ANSWER",
         model::ToastKind::Success},
        {"nowbar-flash", model::Phase::Writing, true, false, false, 60, 20, "Save failed: EACCES",
         model::ToastKind::Error},
    };
}

std::string goldenText(const Scene& scene) {
    // A golden must not inherit the previous check's interaction state. The composer's border is
    // ImGui's nav cursor, and a check that drew frames before this one leaves it visible - which is
    // how a check inserted ahead of the goldens silently changed the idle 100x30 golden during
    // task 39. Nothing in a golden is focused or navigated and the pointer is nowhere, so pinning
    // both off is the honest baseline rather than a mask over a defect.
    ImGui::SetNavCursorVisible(false);
    ImGui::GetIO().MousePos = ImVec2(-FLT_MAX, -FLT_MAX);

    // The scene is drawn through the whole shell, so the golden pins where the row sits
    // and what the rest of the frame does around it.
    tui::TuiState state;
    // A run's sentence ends in `Busy` - the fourth chip's word for all four of the working
    // phases (D writes both, and the splitter keeps whichever it got).
    tui::tuiSetStatusText(
        state, scene.running
                   ? "Context: 42000/128000 tokens | 38.1 tok/s | Model: 'qwen3-coder' | Busy"
                   : "Context: 42000/128000 tokens | 38.1 tok/s | Model: 'qwen3-coder' | Ready");
    state.readyStatus = !scene.running;
    if (scene.running) {
        state.startProcesssingTime = std::chrono::system_clock::now() - std::chrono::seconds(12);
        state.chat.hasStreamMsg = true;
        state.chat.streamMsg.text = "an answer that is still arriving";
    }
    state.ui.offline = scene.offline;
    // The scene's feedback item, raised before the shell derives the row - the frame reads the
    // queue, so this is the same path a copy or a save takes (task 45).
    if (scene.flash != nullptr)
        model::raiseToast(state.ui, model::Toast{scene.flashKind, scene.flash, {}},
                          /*nowMs=*/1000, /*textGrid=*/true);

    // The compact Raw Log scene. An *open* Raw Log is the text grid's full-screen overlay
    // (task 33), so a shell frame with the surface up paints over the row this scene exists to
    // pin. The tiling is drawn first with the surface closed - the derivation is the shell's own,
    // not a fixture rebuilt here - and the row is then drawn on its own with the badge flag set.
    // The screen is therefore the row by itself: one draw, no overdraw, and the badge's pixels
    // are exactly what the row paints.
    const bool rawLogScene = scene.rawLogOpen;
    state.ui.rawLogOpen = false;
    const views::ShellFrame frameOut = shellFrame(state, scene.w, scene.h);
    const ui::Metrics m = ui::metricsFor(true, 1.0f, 1.0f);
    views::NowBarInput input = frameOut.nowBar;
    Grid grid = capture();
    if (rawLogScene) {
        input.rawLogOpen = true;
        g_w = scene.w;
        g_h = scene.h;
        frame([&](const ui::Metrics&) { views::renderNowBar(input, frameOut.layout.nowBar, m); });
        grid = capture();
    }
    // The row the frame *drew* (`ShellFrame::nowBar`), not a fixture rebuilt here: a golden
    // that measured its own copy could drift from the derivation and still "match".
    const views::NowBarParts parts = views::nowBarMeasure(input, frameOut.layout.nowBar, m);

    std::string text = "name: ";
    text += scene.name;
    text += "\nscreen: " + std::to_string(scene.w) + "x" + std::to_string(scene.h);
    text += "\nnowbar: " + partText({frameOut.layout.nowBar.x, frameOut.layout.nowBar.y,
                                     frameOut.layout.nowBar.w, frameOut.layout.nowBar.h});
    text += "\nphase: " + std::string(model::phaseWord(scene.phase));
    text += " compact: " + std::string(frameOut.layout.compact ? "1" : "0");
    text += "\nparts: rule=" + num(parts.rule.w) + " mark=" + num(parts.mark.w) +
            " state=" + num(parts.state.w) + " tail=" + num(parts.tail.w) +
            " hints=" + num(parts.hints.w) + " ctx=" + num(parts.chipCtx.w) +
            " speed=" + num(parts.chipSpeed.w) + " model=" + num(parts.chipModel.w) +
            " stateChip=" + num(parts.chipState.w) + " badge=" + num(parts.badge.w) +
            " truncated=" + (parts.hintsTruncated ? "1" : "0");
    text += "\ntext:\n";
    for (int y = 0; y < grid.ny; ++y) {
        std::string row = grid.rowText(y);
        while (!row.empty() && row.back() == ' ')
            row.pop_back();
        text += row;
        text += '\n';
    }
    text += "fills:\n";
    for (int y = 0; y < grid.ny; ++y) {
        std::string runs;
        int start = -1;
        int index = -1;
        for (int x = 0; x <= grid.nx; ++x) {
            const int value = x < grid.nx ? static_cast<int>(grid.bgAt(x, y)) : -1;
            const bool same = start >= 0 && value == index;
            if (start >= 0 && !same) {
                runs += (runs.empty() ? "" : ", ") + std::to_string(start) + "-" +
                        std::to_string(x - 1) + "=" + std::to_string(index);
                start = -1;
            }
            if (value > 0 && start < 0) {
                start = x;
                index = value;
            } else if (value <= 0) {
                start = -1;
            }
        }
        text += std::to_string(y) + ": " + runs + "\n";
    }
    return text;
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path);
    if (!in.good())
        return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path);
    if (!out.good())
        return false;
    out << text;
    return out.good();
}

// ---------------------------------------------------------------------------
// 13. The ABI: the numbers arrive through the C surface D calls (task 39)
// ---------------------------------------------------------------------------

/// One structured status, filled the way D fills it: the four chips' numbers plus the run's
/// clock and step.
TuiRuntimeStatus abiStatus(int phase, const char* detail) {
    TuiRuntimeStatus status{};
    status.phase = phase;
    status.detail = String{detail, detail ? std::strlen(detail) : 0};
    status.contextUsed = 42000;
    status.contextMax = 128000;
    status.compactAt = 115200;
    status.tokensPerSecond = 38.1;
    status.elapsedMs = 1234;
    status.pendingQuestions = 0;
    return status;
}

void checkRuntimeStatusAbi() {
    // D calls this on every status tick with whatever handle the UI actor holds, so a null state
    // (or a null status) has to be a no-op rather than a crash.
    tuiSetRuntimeStatus(nullptr, nullptr);
    TuiRuntimeStatus probe = abiStatus(1, "");
    tuiSetRuntimeStatus(nullptr, &probe);
    check(true, "tuiSetRuntimeStatus is null-safe in both arguments");

    // The C++ aggregate, wrapped in the ABI's handle - the shape `tui_api.cpp` keeps.
    tui::TuiState state;
    TuiState handle{&state};
    tui::tuiSetStatusText(
        state, "Context: 42000/128000 tokens | 38.1 tok/s | Model: 'qwen3-coder' | Ready");

    // The sentence is still the fallback: a frame with no structured status derives the chips
    // from it, so D's plain `tuiSetStatusText` keeps working.
    shellFrame(state, 100, 30);
    check(state.ui.chips == designChips(), "the sentence's chips are what the row draws");

    // The workbench stores it and formats the four chips from the numbers (§6.3): the identity
    // test, through the surface D actually calls rather than through the model function.
    TuiRuntimeStatus inbound = abiStatus(0, "");
    tuiSetRuntimeStatus(&handle, &inbound);
    check(state.ui.status.structured, "the workbench stores the structured status");
    check(state.ui.chips == designChips(),
          "and its chips are byte-identical to the sentence's for the same numbers");
    check(state.ui.status.contextUsed == 42000 && state.ui.status.contextMax == 128000 &&
              state.ui.status.compactAt == 115200,
          "the context numbers round-trip");
    check(state.ui.status.elapsedMs == 1234, "the ABI clock round-trips");
    check(std::abs(state.ui.status.tokensPerSecond - 38.1) < 1e-9, "the rate round-trips");
    check(state.ui.status.pendingQuestions == 0, "pendingQuestions round-trips");

    // Every String is copied during the call, like every other inbound String in this API: the
    // caller's buffer may go as soon as the call returns.
    std::string callerDetail = "read_file";
    TuiRuntimeStatus withDetail = abiStatus(2, callerDetail.c_str());
    tuiSetRuntimeStatus(&handle, &withDetail);
    callerDetail.assign("something else entirely");
    check(state.ui.status.detail == std::string("read_file"),
          "the detail is copied during the call");
    withDetail.detail = String{"", 0};
    check(state.ui.status.detail == std::string("read_file"),
          "and survives the caller's own buffer");

    // An ordinal this build does not know is idle - not a neighbour, not a crash.
    TuiRuntimeStatus unknown = abiStatus(99, "");
    tuiSetRuntimeStatus(&handle, &unknown);
    check(state.ui.status.phase == model::phaseOrdinal(model::Phase::Idle),
          "an unknown phase clamps to idle");
    check(state.ui.chips.state == std::string("Ready"), "and the state chip says Ready");

    // `offline` is the one phase with a consequence beyond the row: it is what drives the task-8
    // card and the disabled composer, and no sentence can express it. The card is the frame
    // layer's derivation (`tuiRender` calls exactly this), so the fact is asserted where the
    // frame reads it - not by drawing a disabled composer, which would leave its nav border in
    // the state the goldens then capture.
    TuiRuntimeStatus offline = abiStatus(6, "");
    tuiSetRuntimeStatus(&handle, &offline);
    check(state.offline, "phase 6 makes the model unreachable");
    check(state.ui.chips.state == std::string("Offline"), "its state chip says Offline");
    check(model::deriveThreadView(/*hasMessages=*/false, /*loadingSession=*/false, state.offline) ==
              model::ThreadView::Offline,
          "the thread's offline card follows from it");

    // While the numbers own the chips, a frame never re-derives them - and a sentence that
    // changes only re-formats the one chip the struct cannot carry (the model's name).
    tui::tuiSetStatusText(state, "Context: 1/2 tokens | 0.0 tok/s | Model: 'other' | Ready");
    check(state.ui.chips.context == std::string("42k/128k"),
          "a changed sentence keeps the structured numbers");
    check(state.ui.chips.model == std::string("other"), "but re-formats the model chip it owns");
    shellFrame(state, 100, 30);
    check(state.ui.chips.context == std::string("42k/128k"),
          "and a frame leaves the chips alone (no per-frame formatting)");
    check(state.ui.status.contextUsed == 42000, "nor does it overwrite D's numbers");
    check(state.ui.status.elapsedMs == 1234, "nor D's clock");

    // NULL hands the row back to the sentence splitter, and drops the fact only numbers can carry.
    tuiSetRuntimeStatus(&handle, nullptr);
    check(!state.ui.status.structured, "NULL clears the structured status");
    check(!state.offline, "and the offline fact with it");
    check(state.ui.chips == state.statusChips, "the splitter owns the chips again");
    shellFrame(state, 100, 30);
    check(state.ui.chips == state.statusChips, "and the frame keeps projecting them");

    // A session switch clears it too (§5.4): the numbers described the session the user left.
    tuiSetRuntimeStatus(&handle, &inbound);
    check(state.ui.status.structured, "set again before the switch");
    tui::tuiClearOutput(state);
    check(!state.ui.status.structured, "a clear or a session switch drops the status");
    check(state.ui.chips == state.statusChips, "and the row falls back to the sentence");
}

/// The row's clock (task 39, D-STATUS). It runs after the goldens on purpose: a frame that draws
/// a busy composer leaves ImGui's active-item state different, and the idle golden's caret is one
/// cell of that state - a check that draws before the goldens must not feed them.
void checkRuntimeStatusClock() {
    tui::TuiState state;
    TuiState handle{&state};
    TuiRuntimeStatus status = abiStatus(0, "");
    tuiSetRuntimeStatus(&handle, &status);
    check(state.ui.status.structured, "the clock check starts from a structured status");

    // A run that has not ticked for a while (a slow first token, a tool call) keeps the row's
    // clock moving: D's `elapsedMs` is only sampled when a status is released, so the frame's own
    // clock is the live one while the run is in flight.
    state.readyStatus = false;
    state.startProcesssingTime = std::chrono::system_clock::now() - std::chrono::seconds(12);
    const views::ShellFrame running = shellFrame(state, 100, 30);
    check(running.nowBar.elapsedMs >= 12000 && running.nowBar.elapsedMs < 13000,
          "a run's clock is the frame's live one, not the last sampled status value");
    check(state.ui.status.elapsedMs == 1234, "the stored elapsed stays D's sampled value");

    // D's value is the settled duration, and it is what the row shows once the run is over.
    state.readyStatus = true;
    const views::ShellFrame settled = shellFrame(state, 100, 30);
    check(settled.nowBar.elapsedMs == 1234, "and D's settled duration is what idle shows");
}

// ---------------------------------------------------------------------------
// 12. The flash (task 45, design §5.25, FR-29)
// ---------------------------------------------------------------------------

/// A feedback item on the row: it owns the hint slot, wears the severity's mark and ink, and is
/// drawn whole or not at all - the same rule the Raw Log's save answer followed, now for every
/// answer the UI gives (a copy, a save, a settings write).
void checkFlash() {
    views::NowBarInput info = idleRow();
    info.flash = "Copied ANSWER";
    info.flashKind = model::ToastKind::Info;
    const views::NowBarParts infoParts = measureFor(info, 100, 30);
    check(infoParts.flashDrawn, "a flash takes the row's hint slot");
    check(infoParts.hintsText.find("Copied ANSWER") != std::string::npos,
          "the item's own words are drawn (got '" + infoParts.hintsText + "')");
    check(infoParts.hintsText.find(views::kNowBarHintPalette) == std::string::npos,
          "and the hints step aside while it is up");
    check(infoParts.hintsText.find(ui::icon(ui::Icon::Info, ui::textGlyphSet())) !=
              std::string::npos,
          "an info item is drawn with the info mark (NFR-16: a mark beside the word)");
    const Grid infoGrid = drawFor(info, 100, 30);
    const std::string infoRow = infoGrid.rowText(infoGrid.ny - 1);
    check(infoRow.find("Copied ANSWER") != std::string::npos, "the row draws the flash");
    check(infoRow.find(views::kNowBarHintPalette) == std::string::npos,
          "and no palette hint beside it");

    // A success wears the ok mark, and a persistent item names the key that answers it.
    views::NowBarInput saved = idleRow();
    saved.flash = "Saved to /tmp/llmfun-raw.log";
    saved.flashKind = model::ToastKind::Success;
    const views::NowBarParts savedParts = measureFor(saved, 100, 30);
    check(savedParts.hintsText.find(ui::icon(ui::Icon::Ok, ui::textGlyphSet())) !=
              std::string::npos,
          "a success is drawn with the ok mark");
    check(savedParts.hintsText.find(views::kNowBarFlashDismiss) == std::string::npos,
          "a transient item names no dismiss key (it goes away by itself)");

    views::NowBarInput failed = idleRow();
    failed.flash = "Save failed: EACCES";
    failed.flashKind = model::ToastKind::Error;
    const views::NowBarParts failedParts = measureFor(failed, 100, 30);
    check(failedParts.hintsText.find("Save failed: EACCES") != std::string::npos,
          "an error's words are drawn");
    check(failedParts.hintsText.find(views::kNowBarFlashDismiss) != std::string::npos,
          "and it names the key that clears it (got '" + failedParts.hintsText + "')");

    // Nothing on the row times anything: the frame drops an expired item, and until it does the
    // row keeps drawing it - a persistent one for as long as it takes to be answered.
    const Grid twice = drawFor(failed, 100, 30);
    const Grid third = drawFor(failed, 100, 30);
    check(twice.rowText(twice.ny - 1) == third.rowText(third.ny - 1),
          "the row draws the same flash on every frame (the view owns no clock)");

    // The quit question outranks a flash: an open question needs an answer, and a flash is only an
    // answer that was given.
    views::NowBarInput asked = idleRow();
    asked.flash = "Copied ANSWER";
    asked.flashKind = model::ToastKind::Success;
    asked.prompt = tui::kQuitConfirmHint;
    asked.promptShort = tui::kQuitConfirmShort;
    const views::NowBarParts askedParts = measureFor(asked, 100, 30);
    check(!askedParts.flashDrawn, "a question takes the slot from a flash");
    check(askedParts.hintsText == tui::kQuitConfirmHint ||
              askedParts.hintsText == tui::kQuitConfirmShort,
          "and the question is what the row says");

    // Whole or not at all: a flash that cannot fit is dropped and the hints return. The row says
    // nothing rather than the first half of a message.
    views::NowBarInput longFlash = idleRow();
    longFlash.flash = "Save failed: /tmp/a/very/long/path/that/cannot/fit/on/a/sixty-cell/row.log";
    longFlash.flashKind = model::ToastKind::Error;
    const views::NowBarParts narrow = measureFor(longFlash, 60, 20);
    check(!narrow.flashDrawn, "a flash too long for the row is dropped");
    check(narrow.hintsText.find("Save failed") == std::string::npos,
          "and never cut to a fragment (got '" + narrow.hintsText + "')");

    // The derivation: the shell reads the queue's newest item, so a raise anywhere in the frame
    // reaches the row without the view knowing what a toast is.
    tui::TuiState state;
    tui::tuiSetStatusText(
        state, "Context: 42000/128000 tokens | 38.1 tok/s | Model: 'qwen3-coder' | Ready");
    model::raiseToast(state.ui, model::Toast{model::ToastKind::Success, "Copied ANSWER", {}},
                      /*nowMs=*/1000, /*textGrid=*/true);
    const views::ShellFrame frameOut = shellFrame(state, 100, 30);
    check(frameOut.nowBar.flash == std::string("Copied ANSWER"),
          "the shell derives the row's flash from the queue (got '" + frameOut.nowBar.flash + "')");
    check(frameOut.nowBar.flashKind == model::ToastKind::Success, "with the item's severity");
    g_w = 100;
    g_h = 30;
    const Grid shellGrid = frame([&](const ui::Metrics&) {
        views::renderShell(state, ImVec2(static_cast<float>(g_w), static_cast<float>(g_h)));
    });
    check(shellGrid.rowText(shellGrid.ny - 1).find("Copied ANSWER") != std::string::npos,
          "and a whole frame draws it on the row");

    // The `x` key is the frame's answer (task 45, plan/key_gate.md §4i): one press clears the item
    // the row is showing, and it is read as a character (the terminal delivers no key for it).
    //
    // The composer holds the keyboard at startup, and `ui::takeCharacter` refuses while a text
    // field owns it - the plain-key rule (an `x` typed into a draft must stay an `x`, and a
    // warning must not disappear because the user typed). `Esc` on an empty draft with nothing
    // running is the chain's blur step, which is the path a user takes to the row's keys, so the
    // harness presses it rather than reaching around the guard.
    g_w = 100;
    g_h = 30;
    const auto shellFrameNow = [&]() {
        frame([&](const ui::Metrics&) {
            views::renderShell(state, ImVec2(static_cast<float>(g_w), static_cast<float>(g_h)));
        });
    };
    frame(
        [&](const ui::Metrics&) {
            views::renderShell(state, ImVec2(static_cast<float>(g_w), static_cast<float>(g_h)));
        },
        [](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Escape, true); });
    shellFrameNow();
    for (int i = 0; i < 3 && ImGui::GetIO().WantTextInput; ++i)
        shellFrameNow();
    check(!ImGui::GetIO().WantTextInput, "premise: the composer released the keyboard");
    // The harness calls `renderShell()` directly, so the per-frame `Esc` latch the frame layer
    // clears at the top of `tuiRender()` has to be cleared here: the blur press above latched it.
    state.escapeConsumed = false;
    const int before = static_cast<int>(state.ui.toasts.size());
    frame(
        [&](const ui::Metrics&) {
            views::renderShell(state, ImVec2(static_cast<float>(g_w), static_cast<float>(g_h)));
        },
        [](ImGuiIO& io) { io.AddInputCharactersUTF8("x"); });
    check(static_cast<int>(state.ui.toasts.size()) == before - 1,
          "`x` acknowledges the item the row is showing (got " +
              std::to_string(state.ui.toasts.size()) + " left)");

    // Two items up (a pixel-depth queue, but the *row* is what is asked): the row names the newest,
    // so `x` must answer that one. Popping the oldest removed an item the user could not see and
    // left the one the row was advertising - the review's finding 1, which the one-item case above
    // cannot see because the two readings coincide there.
    state.ui.toasts.clear();
    model::raiseToast(state.ui, model::Toast{model::ToastKind::Error, "the warning", {}},
                      /*nowMs=*/0, /*textGrid=*/false);
    model::raiseToast(state.ui, model::Toast{model::ToastKind::Success, "the copy", {}},
                      /*nowMs=*/0, /*textGrid=*/false);
    check(state.ui.toasts.size() == 2, "premise: two items are up");
    const views::ShellFrame two = shellFrame(state, 100, 30);
    check(two.nowBar.flash == std::string("the copy"),
          "the row names the newest item (got '" + two.nowBar.flash + "')");
    state.escapeConsumed = false;
    frame(
        [&](const ui::Metrics&) {
            views::renderShell(state, ImVec2(static_cast<float>(g_w), static_cast<float>(g_h)));
        },
        [](ImGuiIO& io) { io.AddInputCharactersUTF8("x"); });
    check(state.ui.toasts.size() == 1 && state.ui.toasts.back().title == "the warning",
          "`x` answers the item the row named, not the one behind it (got " +
              std::to_string(state.ui.toasts.size()) + " left)");

    // The save's answer comes through the same queue (task 45): the Raw Log's surface writes one
    // notice, the frame publishes it as one item, and nothing else prints it - the two
    // presentations read the same queue. The surface's answers are applied at the *end* of the
    // frame (`applyRawLogAnswers`), so the item reaches the row on the next one, like every other
    // answer in this frame.
    state.ui.toasts.clear();
    state.rawLog.notice = "Saved to /tmp/llmfun-raw.log";
    state.rawLog.noticeKind = model::ToastKind::Success;
    shellFrame(state, 100, 30);
    const views::ShellFrame savedFrame = shellFrame(state, 100, 30);
    check(state.ui.toasts.size() == 1,
          "one save is one item (got " + std::to_string(state.ui.toasts.size()) + ")");
    check(savedFrame.nowBar.flash == std::string("Saved to /tmp/llmfun-raw.log"),
          "and the row says where it went (got '" + savedFrame.nowBar.flash + "')");
    check(state.rawLog.notice.empty(), "the surface's notice is consumed once, never duplicated");

    // An empty queue is an empty slot: no flash, the hints are back.
    model::dismissShownToast(state.ui);
    const views::ShellFrame cleared = shellFrame(state, 100, 30);
    check(cleared.nowBar.flash.empty(), "an acknowledged item leaves no flash behind");
    check(drawFor(cleared.nowBar, 100, 30).hasText(views::kNowBarHintPalette),
          "and the hints return to the row");
}

void checkGoldens(bool update) {
    const std::string dir = TEST_NOWBAR_GOLDEN_DIR;
    for (const Scene& scene : scenes()) {
        const std::string path = dir + "/" + scene.name + "-" + std::to_string(scene.w) + "x" +
                                 std::to_string(scene.h) + ".txt";
        const std::string text = goldenText(scene);
        if (update) {
            check(writeFile(path, text), "golden written: " + path);
            continue;
        }
        std::string expected;
        if (!readFile(path, expected)) {
            check(false, "golden is readable: " + path + " (run with --update to create it)");
            continue;
        }
        if (expected == text) {
            check(true, std::string("the ") + scene.name + " " + std::to_string(scene.w) + "x" +
                            std::to_string(scene.h) + " golden matches");
            continue;
        }
        std::size_t line = 1;
        std::size_t at = 0;
        while (at < expected.size() && at < text.size() && expected[at] == text[at]) {
            line += expected[at] == '\n' ? 1 : 0;
            ++at;
        }
        check(false, std::string("the ") + scene.name + " " + std::to_string(scene.w) + "x" +
                         std::to_string(scene.h) + " golden matches (first difference on line " +
                         std::to_string(line) + ": " + path + ")");
    }
}

} // namespace

int main(int argc, char** argv) {
    bool update = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0)
            g_verbose = true;
        else if (std::strcmp(argv[i], "--update") == 0)
            update = true;
        else if (std::strcmp(argv[i], "--print") == 0)
            g_print = true;
        else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0)
            usage(argv[0], "print every check with -v, rewrite the goldens with --update, "
                           "dump the grids with --print");
        else
            usage(argv[0], "unknown argument");
    }

    // One glyph set for every frame, whatever terminal runs this: `LLMFUN_ASCII=0` outranks
    // TERM and the locale in the detection order, so a golden cannot depend on the
    // environment.
    ::setenv("LLMFUN_ASCII", "0", 1);

    ImGui::CreateContext();
    ImTui_ImplText_Init();

    checkSource();
    checkVocabulary();
    checkChips();
    checkMapping();
    checkClock();
    checkLadder();
    checkHints();
    checkOverlayHint();
    checkBadge();
    checkFocusHint();
    checkFocusStopHint();
    checkColours();
    checkScreen();
    checkShell();
    checkStopHint();
    checkQuitPrompt();
    checkRuntimeStatusAbi();
    checkGoldens(update);
    // The flash checks run *after* the goldens: they deliberately blur the composer (`Esc`, the
    // route to a character key) and the field's focus ring is part of what the goldens pin - a
    // check that draws before them must not feed them (the same rule `checkRuntimeStatusClock`
    // follows).
    checkFlash();
    checkRuntimeStatusClock();

    ImGui::DestroyContext();

    if (g_failures == 0) {
        std::printf("test_nowbar: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "test_nowbar: %d of %d checks failed\n", g_failures, g_checks);
    return 1;
}
