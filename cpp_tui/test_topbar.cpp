// test_topbar.cpp
//
// The top bar (views/topbar.h, design §4.2 / FR-02): five states, one degradation, one
// inline rename, and the window-title mirror it feeds.
//
// What this binary proves, in order:
//
//   1. the source. `views/topbar.cpp` writes no colour literal, takes the profile from the
//      metrics (never `tuiIsTextGrid()`), reads no display size, names no v4 type and does not
//      include `tui.h` - the layering the rest of `views/` is held to (`tools/gate_ratchet.txt`);
//   2. the labels. `elapsedLabel()`/`pillLabel()` are pinned for every state, including the
//      clock-jump and past-an-hour cases;
//   3. the arithmetic. `topBarMeasure()` is swept over widths in both profiles: every part sits
//      inside the bar, no two parts overlap, the title takes the remainder, and the three
//      safety signals (the mark, the run pill, the `RAW` badge) survive where the hint and the
//      wordmark do not;
//   4. the truncation. A long title is cut with the profile's ellipsis and the rest of the bar
//      keeps its geometry;
//   5. the colours. Every pill pair the bar can draw clears the design's 4.5:1 text bar;
//   6. the rename. A click opens the box for the *clicked* session, typing reaches the buffer,
//      Enter commits exactly once, `Esc` cancels and marks the press consumed, an empty title
//      is refused, and a bar that may not rename (or may not take keys) does neither;
//   7. the title seam. The internal `tuiBackendSetWindowTitle()` forwards a *change* only, maps
//      "" to the application name, and is a no-op with no sink installed (the cell grid);
//   8. the shell. One real frame: the bar reads the active session's title, the window title
//      mirrors it, and a rename from the bar queues exactly one `SessionAction{Rename, id,
//      title}` through `queueSessionAction()` - the rail's own call;
//   9. the goldens. One 100x30 text screen per state plus a narrow 60x20 one, each pinning the
//      measured parts and the painted fills as well as the glyphs.
//
// The bar is drawn through the real text backend (the `TScreen` pattern of
// `test_session_filter_smoke.cpp`), so a golden is what the terminal would show, not what the
// drawing code believes it drew.
//
// Exit codes: 0 = all checks pass; 1 = a check failed; 2 = usage error.
//
//   ./test_topbar [-v] [--update]
//
// `--update` rewrites the fixtures; do that only for a deliberate rendering change, and commit
// the diff with it. They are captured with `LLMFUN_ASCII=0`, so a CJK locale or `TERM=linux`
// cannot change a golden.

#include "views/shell.h"
#include "views/topbar.h"

#include "tui.h"
#include "tui_backend.h"
#include "ui/icons.h"
#include "ui/layout.h"
#include "ui/metrics.h"
#include "ui/primitives.h"
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

namespace ui = llmfun::tui::ui;
namespace views = llmfun::tui::views;
namespace tui = llmfun::tui;

namespace {

int g_checks = 0;
int g_failures = 0;
bool g_verbose = false;

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
    std::fprintf(stderr, "usage: %s [-v] [--update]   (%s)\n", argv0, problem);
    std::exit(2);
}

/// `4`, not `4.0`: every length is a whole number of cells or pixels, so a fractional one has to
/// look like the bug it is in a failure message.
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

std::string partText(const views::TopBarPart& part) {
    return num(part.x) + "," + num(part.y) + " " + num(part.w) + "x" + num(part.h);
}

// ---------------------------------------------------------------------------
// The cell grid the text backend writes
// ---------------------------------------------------------------------------

ImTui::TScreen g_screen;

constexpr int kScreenW = 100;
constexpr int kScreenH = 30;

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
    bool blank(int x, int y) const { return at(x, y) == 0; }

    std::string rowText(int y) const {
        std::string text;
        for (int x = 0; x < nx; ++x) {
            const uint32_t cp = at(x, y);
            text += cp == 0 ? std::string(" ") : utf8Of(cp);
        }
        return text;
    }

    /// The row's text starting at `x`, `cells` wide - how a label is read back out of the grid
    /// instead of being compared with the string the drawing code used.
    std::string span(int x, int y, int cells) const {
        std::string text;
        for (int i = 0; i < cells; ++i) {
            const uint32_t cp = at(x + i, y);
            text += cp == 0 ? std::string(" ") : utf8Of(cp);
        }
        return text;
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

/// One frame of the text grid: a decoration-free root window filling the display, the body
/// drawing inside it, then the frame rendered through the real backend.
///
/// `inject` runs *before* `NewFrame`, because that is when ImGui consumes its input queue - an
/// event added after it would only be seen by the next frame.
Grid frame(const std::function<void(const ui::Metrics&)>& body,
           const std::function<void(ImGuiIO&)>& inject = nullptr) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(kScreenW), static_cast<float>(kScreenH));
    io.DeltaTime = 1.0f / 60.0f;
    if (inject)
        inject(io);
    ImTui_ImplText_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(kScreenW), static_cast<float>(kScreenH)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("topbar-frame", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus);
    body(ui::metricsFor(true, 1.0f, 1.0f));
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::Render();
    ImTui_ImplText_RenderDrawData(ImGui::GetDrawData(), &g_screen);
    return capture();
}

void pressKey(ImGuiKey key, const std::function<void(const ui::Metrics&)>& body) {
    frame(body, [&](ImGuiIO& io) { io.AddKeyEvent(key, true); });
    frame(body, [&](ImGuiIO& io) { io.AddKeyEvent(key, false); });
}

void typeText(const std::string& text, const std::function<void(const ui::Metrics&)>& body) {
    for (const char c : text)
        frame(body, [&](ImGuiIO& io) { io.AddInputCharacter(static_cast<unsigned short>(c)); });
}

// ---------------------------------------------------------------------------
// Fixtures and helpers
// ---------------------------------------------------------------------------

ui::Rect barAt(float x, float y, float w, float h, const ui::Metrics& m) {
    return ui::Rect{x, y, w, h, m.textGrid};
}

/// The bar the layout engine hands the view at `w x h`, so every check uses the geometry the
/// frame really answers for - not a rectangle this harness invented.
ui::Rect shellTopBar(int w, int h, const ui::Metrics& m) {
    ui::LayoutPrefs prefs;
    prefs.railOpen = true;
    prefs.inspectorOpen = true;
    const ui::ShellLayout layout =
        ui::computeShell(m, ImVec2(static_cast<float>(w), static_cast<float>(h)), prefs);
    return layout.topBar;
}

views::TopBarState stateFor(views::RunState run, const char* title = "Demo session") {
    views::TopBarState state;
    state.sessionId = "s1";
    state.sessionTitle = title;
    state.run = run;
    if (run == views::RunState::Running || run == views::RunState::Waiting)
        state.elapsedMs = 12000; // 00:12, the design's own pill
    return state;
}

/// Two rects share at least one cell (or pixel).
bool overlaps(const views::TopBarPart& a, const views::TopBarPart& b) {
    if (a.empty() || b.empty())
        return false;
    return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}

bool inside(const views::TopBarPart& part, const ui::Rect& box) {
    if (part.empty())
        return true;
    return part.x >= box.x - 0.01f && part.y >= box.y - 0.01f &&
           part.right() <= box.right() + 0.01f && part.bottom() <= box.bottom() + 0.01f;
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

std::string hex(ImVec4 c) {
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "#%02X%02X%02X", int(c.x * 255.0f + 0.5f),
                  int(c.y * 255.0f + 0.5f), int(c.z * 255.0f + 0.5f));
    return buffer;
}

// ---------------------------------------------------------------------------
// 1. The source obeys the layering rules
// ---------------------------------------------------------------------------

int countOccurrences(const std::string& haystack, const std::string& needle) {
    int count = 0;
    for (std::size_t at = haystack.find(needle); at != std::string::npos;
         at = haystack.find(needle, at + needle.size()))
        ++count;
    return count;
}

void checkSource() {
    const char* path = TEST_TOPBAR_SOURCE;
    std::ifstream in(path);
    check(in.good(), std::string("views/topbar.cpp is readable (") + path + ")");
    if (!in.good())
        return;
    std::string source;
    std::string line;
    while (std::getline(in, line)) {
        source += line;
        source += '\n';
    }
    check(countOccurrences(source, "ImVec4(") == 0 && countOccurrences(source, "ImVec4{") == 0,
          "views/topbar.cpp builds no ImVec4 value: colours come from ui::color()");
    check(countOccurrences(source, "IM_COL32(") == 0,
          "views/topbar.cpp writes no packed colour literal");
    check(countOccurrences(source, "tuiIsTextGrid") == 0,
          "views/topbar.cpp takes the profile from Metrics, never from tuiIsTextGrid()");
    check(countOccurrences(source, "DisplaySize") == 0,
          "views/topbar.cpp lays out inside the rect it is given, not against the display");
    check(countOccurrences(source, "ChatMessage") == 0 &&
              countOccurrences(source, "RenderGroup") == 0,
          "views/topbar.cpp names no v4 type (D-ADAPTER)");
    check(countOccurrences(source, "#include \"tui.h\"") == 0,
          "views/topbar.cpp does not include tui.h: the bar reads no aggregate");
}

// ---------------------------------------------------------------------------
// 2. Labels
// ---------------------------------------------------------------------------

void checkLabels() {
    check(views::elapsedLabel(0) == "00:00", "0 ms is 00:00");
    check(views::elapsedLabel(12000) == "00:12", "12 s is 00:12 (the design's pill)");
    check(views::elapsedLabel(61000) == "01:01", "61 s is 01:01");
    check(views::elapsedLabel(3599000) == "59:59", "just under an hour stays mm:ss");
    check(views::elapsedLabel(3600000) == "1:00:00", "an hour switches to h:mm:ss");
    check(views::elapsedLabel(3661000) == "1:01:01", "past an hour the minutes keep their place");
    check(views::elapsedLabel(-5) == "00:00", "a negative clock reads 00:00, never -00:00");

    check(views::pillLabel(views::RunState::Idle, 12000) == "Ready",
          "idle says Ready and does not print a clock");
    check(views::pillLabel(views::RunState::Running, 12000) == "running 00:12",
          "running prints the run state and its clock");
    check(views::pillLabel(views::RunState::Waiting, 12000) == "waiting 00:12",
          "waiting keeps the clock it waited through");
    check(views::pillLabel(views::RunState::Failed, 12000) == "failed",
          "failed prints its word and no clock");

    check(std::strcmp(views::runStateName(views::RunState::Idle), "idle") == 0 &&
              std::strcmp(views::runStateName(views::RunState::Running), "running") == 0 &&
              std::strcmp(views::runStateName(views::RunState::Waiting), "waiting") == 0 &&
              std::strcmp(views::runStateName(views::RunState::Failed), "failed") == 0 &&
              std::strcmp(views::runStateName(views::RunState::RawLogOpen), "rawLogOpen") == 0,
          "every run state has its own name");

    const views::TopBarState running = stateFor(views::RunState::Running);
    check(views::barState(running) == views::RunState::Running,
          "a bar with the raw log closed is the run's state");
    views::TopBarState raw = running;
    raw.rawLogOpen = true;
    check(views::barState(raw) == views::RunState::RawLogOpen,
          "an open raw log is the bar's fifth state");
    check(views::pillLabel(raw.run, raw.elapsedMs) == "running 00:12",
          "the pill keeps reporting the run while the badge reports the log");
}

// ---------------------------------------------------------------------------
// 3. The arithmetic
// ---------------------------------------------------------------------------

void checkSweepFor(const ui::Metrics& m, const char* profile) {
    const views::RunState states[] = {views::RunState::Idle, views::RunState::Running,
                                      views::RunState::Waiting, views::RunState::Failed};
    bool allInside = true;
    bool noneOverlap = true;
    bool rightmost = true;
    bool ordered = true;
    std::string firstProblem;

    const int sw = m.textGrid ? 1 : 8; // one unit of the profile, for the width sweep
    for (int units = 40; units <= 160; units += 8) {
        const float w = static_cast<float>(units) * static_cast<float>(sw);
        const ui::Rect bar = barAt(0.0f, 0.0f, w, m.topBarH, m);
        for (const views::RunState run : states) {
            for (int rawLog = 0; rawLog < 2; ++rawLog) {
                views::TopBarState state = stateFor(run);
                state.rawLogOpen = rawLog != 0;
                views::TopBarState longTitle = state;
                longTitle.sessionTitle =
                    "a session title that is very much longer than any bar can hold in one row";

                for (const views::TopBarState& candidate : {state, longTitle}) {
                    const views::TopBarParts parts = views::topBarMeasure(candidate, bar, m);
                    const views::TopBarPart all[] = {parts.mark,  parts.wordmark, parts.rule,
                                                     parts.title, parts.hint,     parts.pill,
                                                     parts.badge, parts.gear};
                    for (const views::TopBarPart& part : all) {
                        if (!inside(part, bar)) {
                            allInside = false;
                            if (firstProblem.empty())
                                firstProblem = std::string(profile) + " width " + num(w) +
                                               " units: " + partText(part) + " escapes the bar";
                        }
                    }
                    for (std::size_t i = 0; i < sizeof all / sizeof all[0]; ++i)
                        for (std::size_t j = i + 1; j < sizeof all / sizeof all[0]; ++j)
                            if (overlaps(all[i], all[j])) {
                                noneOverlap = false;
                                if (firstProblem.empty())
                                    firstProblem = std::string(profile) + " width " + num(w) +
                                                   " units: two parts overlap";
                            }
                    // The right cluster is anchored on the bar's right edge (minus one pad), so
                    // the bar is anchored on both sides rather than left-aligned with a hole.
                    const views::TopBarPart& last = !parts.gear.empty()    ? parts.gear
                                                    : !parts.badge.empty() ? parts.badge
                                                                           : parts.pill;
                    if (std::fabs((bar.right() - m.pad1) - last.right()) > 0.01f) {
                        rightmost = false;
                        if (firstProblem.empty())
                            firstProblem =
                                std::string(profile) + " width " + num(w) +
                                " units: the right cluster does not end at the bar's edge";
                    }
                    // Left to right: mark, wordmark, rule, title, hint, pill, badge, gear.
                    const views::TopBarPart order[] = {parts.mark,  parts.wordmark, parts.rule,
                                                       parts.title, parts.hint,     parts.pill,
                                                       parts.badge, parts.gear};
                    float previous = -1.0f;
                    for (const views::TopBarPart& part : order) {
                        if (part.empty())
                            continue;
                        if (part.x + 0.01f < previous) {
                            ordered = false;
                            if (firstProblem.empty())
                                firstProblem = std::string(profile) + " width " + num(w) +
                                               " units: the parts are out of order";
                        }
                        previous = part.right();
                    }
                }
            }
        }
    }
    check(allInside, std::string(profile) + ": every part sits inside the bar" +
                         (allInside ? "" : " - " + firstProblem));
    check(noneOverlap, std::string(profile) + ": no two parts overlap" +
                           (noneOverlap ? "" : " - " + firstProblem));
    check(rightmost, std::string(profile) + ": the run pill ends at the bar's right edge");
    check(ordered, std::string(profile) + ": the parts keep the design's left-to-right order");
}

void checkMeasureParts() {
    const ui::Metrics m = ui::metricsFor(true, 1.0f, 1.0f);
    const ui::Rect bar = shellTopBar(kScreenW, kScreenH, m);
    check(bar.h == m.row && bar.w == kScreenW, "the layout gives the bar one full-width row (" +
                                                   partText({bar.x, bar.y, bar.w, bar.h}) + ")");

    const views::TopBarState idle = stateFor(views::RunState::Idle);
    const views::TopBarParts parts = views::topBarMeasure(idle, bar, m);
    check(!parts.mark.empty() && !parts.wordmark.empty() && !parts.title.empty() &&
              !parts.hint.empty() && !parts.pill.empty() && !parts.gear.empty(),
          "a 100-cell bar draws every part");
    check(parts.badge.empty(), "the RAW badge is absent while the raw log is closed");
    check(!views::topBarMeasure(stateFor(views::RunState::Idle, ""), bar, m).titleTruncated,
          "an empty title is not 'truncated'");
    check(parts.titleText == "Demo session", "a title that fits is drawn whole");
    check(!parts.titleTruncated, "a title that fits is not marked as truncated");

    // The title is the flexible part: it is exactly the room between the two clusters.
    check(parts.title.x > parts.mark.x && parts.title.right() < parts.hint.x,
          "the title sits between the wordmark and the hint");
    check(std::fabs((parts.hint.x - m.pad1) - parts.title.right()) < 0.01f,
          "one pad separates the title from the hint");

    // The badge is a safety signal: its width appears and disappears with the flag, and it never
    // covers the pill.
    views::TopBarState raw = idle;
    raw.rawLogOpen = true;
    const views::TopBarParts withBadge = views::topBarMeasure(raw, bar, m);
    check(!withBadge.badge.empty(), "the RAW badge is drawn exactly while the raw log is open");
    check(withBadge.badge.x == withBadge.pill.right() + m.pad1,
          "the badge sits one pad right of the pill");
    check(withBadge.gear.x == withBadge.badge.right() + m.pad1,
          "the settings entry sits one pad right of the badge");

    // The pill grows with its label, and the bar is not re-measured behind the caller's back.
    const views::TopBarParts running =
        views::topBarMeasure(stateFor(views::RunState::Running), bar, m);
    check(running.pill.w == ui::measurePill(m, "running 00:12").w,
          "the pill's width is its own measure (running 00:12)");
    check(views::topBarMeasure(stateFor(views::RunState::Idle), bar, m).pill.w ==
              ui::measurePill(m, "Ready").w,
          "the idle pill is the measure of Ready");
}

void checkNarrowBar() {
    const ui::Metrics m = ui::metricsFor(true, 1.0f, 1.0f);
    // The design's narrow breakpoint: 60 cells, where the guideline is "the title truncates, the
    // mark and the run pill survive".
    const ui::Rect bar = shellTopBar(60, 20, m);
    check(bar.w == 60.0f && bar.h == m.row, "a 60x20 display still gives the bar one full row");

    views::TopBarState state = stateFor(views::RunState::Running);
    state.sessionTitle =
        "a session title that is very much longer than any bar can hold in one row";
    const views::TopBarParts parts = views::topBarMeasure(state, bar, m);
    check(!parts.mark.empty(), "60 cells: the wordmark's mark survives");
    check(!parts.pill.empty(), "60 cells: the run pill survives");
    check(parts.badge.empty(), "60 cells: no RAW badge while the raw log is closed");
    check(!parts.title.empty(),
          "60 cells: the title keeps a box (it truncates, it does not vanish)");
    check(parts.titleTruncated, "60 cells: the long title is marked truncated");
    check(ui::displayWidth(parts.titleText) <= static_cast<int>(parts.title.w),
          "60 cells: the cut title fits its box");
    check(parts.titleText.size() < state.sessionTitle.size(),
          "60 cells: the cut title is shorter than the original");
    check(parts.titleText.find("\xE2\x80\xA6") != std::string::npos,
          "60 cells: the cut is marked with the ellipsis glyph");
    check(parts.hint.empty(), "60 cells: the palette hint is the first thing to drop");

    // A bar narrower than the fixed parts: the title goes to zero and nothing escapes.
    for (int cells = 40; cells <= 100; ++cells) {
        const ui::Rect narrow = barAt(0.0f, 0.0f, static_cast<float>(cells), 1.0f, m);
        const views::TopBarParts tight = views::topBarMeasure(state, narrow, m);
        const views::TopBarPart all[] = {tight.mark, tight.wordmark, tight.rule,  tight.title,
                                         tight.hint, tight.pill,     tight.badge, tight.gear};
        bool ok = true;
        for (const views::TopBarPart& part : all)
            ok = ok && inside(part, narrow);
        if (!ok) {
            check(false, std::to_string(cells) + " cells: a part escapes the bar");
            return;
        }
    }
    check(true, "40..100 cells: every part stays inside the bar");
}

// ---------------------------------------------------------------------------
// 5. Colours
// ---------------------------------------------------------------------------

void checkContrast() {
    // Pinned here rather than trusted in a comment: the pill's ink/fill pair is the one choice the
    // view makes that could make a state unreadable.
    const ImVec4 fill = ui::color(ui::ColorToken::raised);
    const struct {
        views::RunState run;
        ui::ColorToken ink;
        const char* name;
    } pairs[] = {
        {views::RunState::Idle, ui::ColorToken::textMid, "idle"},
        {views::RunState::Running, ui::ColorToken::accent, "running"},
        {views::RunState::Waiting, ui::ColorToken::stateWarn, "waiting"},
        {views::RunState::Failed, ui::ColorToken::stateDanger, "failed"},
    };
    for (const auto& pair : pairs) {
        const float ratio = contrastRatio(ui::color(pair.ink), fill);
        check(ratio >= 4.5f, std::string("the ") + pair.name + " pill clears the text bar (" +
                                 hex(ui::color(pair.ink)) + " on " + hex(fill) + " = " +
                                 num(ratio) + ":1)");
    }
    const float badge =
        contrastRatio(ui::color(ui::ColorToken::stateWarn), ui::color(ui::ColorToken::raised));
    check(badge >= 4.5f, "the RAW badge clears the text bar (" + num(badge) + ":1)");
    const float hint =
        contrastRatio(ui::color(ui::ColorToken::textLow), ui::color(ui::ColorToken::surface));
    check(hint >= 4.5f, "the palette hint is the dimmest ink that still clears 4.5:1 (" +
                            num(hint) + ":1 on surface)");
}

// ---------------------------------------------------------------------------
// 6. The inline rename
// ---------------------------------------------------------------------------

void checkRename() {
    const ui::Metrics m = ui::metricsFor(true, 1.0f, 1.0f);
    const ui::Rect bar = shellTopBar(kScreenW, kScreenH, m);
    views::TopBarState state = stateFor(views::RunState::Idle);
    const views::TopBarParts parts = views::topBarMeasure(state, bar, m);
    const float x = parts.title.x + 2.0f;
    const float y = parts.title.y + 0.0f;
    const auto body = [&](const ui::Metrics& metrics) { views::renderTopBar(state, bar, metrics); };

    frame(body, [&](ImGuiIO& io) { io.AddMousePosEvent(x, y); });
    frame(body, [&](ImGuiIO& io) {
        io.AddMousePosEvent(x, y);
        io.AddMouseButtonEvent(0, true);
    });
    frame(body, [&](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); });
    check(state.renaming, "a click on the title opens the rename box");
    check(state.renameId == "s1", "the box belongs to the session that was clicked");
    check(std::string(state.renameBuf.data()) == "Demo session",
          "the box opens with the current title in it");

    // The field takes the keyboard with the whole title selected, so the rename gesture is
    // click, type, Enter: the typed text *replaces* the old title instead of landing in the
    // middle of it.
    frame(body);
    typeText("Renamed", body);
    check(std::string(state.renameBuf.data()) == "Renamed",
          "typing replaces the selected title (" + std::string(state.renameBuf.data()) + ")");

    pressKey(ImGuiKey_Enter, body);
    check(state.renameCommitted, "Enter commits the rename");
    check(state.renamedTitle == "Renamed",
          "the commit carries the edited title (" + state.renamedTitle + ")");
    check(!state.renaming, "the box closes on a commit");
    state.renameCommitted = false;
    state.renamedTitle.clear();
    pressKey(ImGuiKey_Enter, body);
    check(!state.renameCommitted, "a second Enter commits nothing: one press, one action");

    // The title line comes back the frame after the commit - no empty box in between.
    const Grid afterCommit = frame(body);
    check(afterCommit.rowText(0).find("Demo session") != std::string::npos,
          "the title is drawn again the frame after a commit ('" + afterCommit.rowText(0) + "')");

    // Esc cancels and claims the press, so the frame-level Esc steps stand down.
    state = stateFor(views::RunState::Idle);
    frame(body, [&](ImGuiIO& io) {
        io.AddMousePosEvent(x, y);
        io.AddMouseButtonEvent(0, true);
    });
    frame(body, [&](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); });
    frame(body);
    check(state.renaming, "the box is open again for the Esc case");
    pressKey(ImGuiKey_Escape, body);
    check(!state.renaming, "Esc closes the box");
    check(!state.renameCommitted, "Esc commits nothing");
    check(state.escapeConsumed, "Esc marks the press consumed (one step per press)");
    check(state.sessionTitle == "Demo session", "Esc leaves the title unchanged");
    state.escapeConsumed = false;
    pressKey(ImGuiKey_Escape, body);
    check(!state.escapeConsumed, "with the box closed, Esc reaches the frame again");

    // An overlay that owns the keyboard owns Escape: the box keeps it.
    state = stateFor(views::RunState::Idle);
    state.renaming = true;
    state.renameId = "s1";
    state.acceptKeys = false;
    pressKey(ImGuiKey_Escape, body);
    check(state.renaming && !state.escapeConsumed,
          "while an overlay owns the keyboard, Esc does not close the box");

    // An empty title is refused and the box stays open with the draft. The field has to *hold the
    // keyboard* for this: `EnterReturnsTrue` never fires on an unfocused `InputText`, so a check
    // that only hand-sets the buffer proves nothing about the guard. The empty state is reached the
    // way a user reaches it - open the box, select all, delete - because a write to an active field
    // is discarded by the widget (and a hand-set `renameFocus` on a fresh state does not take the
    // keyboard: nothing has given the window focus yet).
    state = stateFor(views::RunState::Idle);
    frame(body, [&](ImGuiIO& io) { io.AddMousePosEvent(x, y); });
    frame(body, [&](ImGuiIO& io) {
        io.AddMousePosEvent(x, y);
        io.AddMouseButtonEvent(0, true);
    });
    frame(body, [&](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); });
    frame(body);
    check(state.renaming, "the box is open for the empty-title case");
    const auto chord = [&](ImGuiKey mod, ImGuiKey key) {
        frame(body, [&](ImGuiIO& io) {
            io.AddKeyEvent(mod, true);
            io.AddKeyEvent(key, true);
        });
        frame(body, [&](ImGuiIO& io) {
            io.AddKeyEvent(key, false);
            io.AddKeyEvent(mod, false);
        });
    };
    chord(ImGuiMod_Ctrl, ImGuiKey_A);
    pressKey(ImGuiKey_Backspace, body);
    check(std::string(state.renameBuf.data()).empty(),
          "select-all and delete empties the draft ('" + std::string(state.renameBuf.data()) +
              "')");
    pressKey(ImGuiKey_Enter, body);
    check(!state.renameCommitted, "an empty title is never queued");
    check(state.renaming, "the box stays open after a refused title");

    // The same shape with the field *not* focused commits nothing either - which is what makes it
    // the discriminator for the check above rather than a second way to say the same thing.
    state = stateFor(views::RunState::Idle);
    state.renaming = true;
    state.renameId = "s1";
    state.renameSeq = 102;
    std::snprintf(state.renameBuf.data(), state.renameBuf.size(), "typed, never focused");
    state.renameFocus = false;
    state.renameHadFocus = false;
    pressKey(ImGuiKey_Enter, body);
    check(!state.renameCommitted,
          "an unfocused box commits nothing (Enter never reaches the field)");

    // A rename the run would refuse is not queued either (see `checkShell`: a busy bar does not
    // open a box at all, so the run has to start while the field is up, and the shell is where that
    // sequence is reachable with a focused field).

    // No session, no target: the title is not a hit box.
    state = stateFor(views::RunState::Idle);
    state.sessionId.clear();
    frame(body, [&](ImGuiIO& io) {
        io.AddMousePosEvent(x, y);
        io.AddMouseButtonEvent(0, true);
    });
    frame(body, [&](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); });
    check(!state.renaming, "without a session the title opens nothing to rename");

    // An overlay that owns the keyboard also owns a click on the bar underneath: the frame's
    // answer (`acceptKeys`) gates the hit box, not just the Esc.
    state = stateFor(views::RunState::Idle);
    state.acceptKeys = false;
    frame(body, [&](ImGuiIO& io) { io.AddMousePosEvent(x, y); });
    frame(body, [&](ImGuiIO& io) {
        io.AddMousePosEvent(x, y);
        io.AddMouseButtonEvent(0, true);
    });
    frame(body, [&](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); });
    check(!state.renaming, "a bar that may not take the keyboard does not open the box on a click");

    // Clicking away terminates the box (the rail's focus-loss rule): an unfocused edit field
    // parked over the session title is not a state anyone asked for.
    state = stateFor(views::RunState::Idle);
    frame(body, [&](ImGuiIO& io) { io.AddMousePosEvent(x, y); });
    frame(body, [&](ImGuiIO& io) {
        io.AddMousePosEvent(x, y);
        io.AddMouseButtonEvent(0, true);
    });
    frame(body, [&](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); });
    frame(body); // the field takes the keyboard
    check(state.renaming, "the box is open before the click-away");
    frame(body, [&](ImGuiIO& io) {
        io.AddMousePosEvent(40.0f, 12.0f); // the thread area, far from the bar
        io.AddMouseButtonEvent(0, true);
    });
    frame(body, [&](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); });
    check(!state.renaming, "a click away from the box terminates it");
    check(!state.renameCommitted, "and commits nothing");

    // A title of up to 255 bytes opens filled (the rail's buffer size, so both entry points
    // behave the same); a longer one opens empty rather than silently truncated.
    state = stateFor(views::RunState::Idle);
    state.sessionTitle = std::string(200, 't');
    frame(body, [&](ImGuiIO& io) {
        io.AddMousePosEvent(x, y);
        io.AddMouseButtonEvent(0, true);
    });
    frame(body, [&](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); });
    check(state.renaming && std::string(state.renameBuf.data()).size() == 200,
          "a 200-byte title opens filled in the bar (" +
              std::to_string(std::string(state.renameBuf.data()).size()) + " bytes)");

    state = stateFor(views::RunState::Idle);
    state.sessionTitle = std::string(300, 't');
    frame(body, [&](ImGuiIO& io) {
        io.AddMousePosEvent(x, y);
        io.AddMouseButtonEvent(0, true);
    });
    frame(body, [&](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); });
    check(state.renaming && state.renameBuf[0] == '\0',
          "a title too long for the buffer opens empty (no silent truncation)");
}

// ---------------------------------------------------------------------------
// 7. The window-title seam
// ---------------------------------------------------------------------------

std::vector<std::string> g_sinkTitles;

void recordTitle(const std::string& title) { g_sinkTitles.push_back(title); }

void checkTitleSeam() {
    // The application's own name has one definition (the seam's fallback is also the title a
    // pixel backend opens its window with), so a cleared title and a fresh window cannot differ.
    check(std::strcmp(tui::applicationWindowTitle(), "llmfun") == 0,
          "applicationWindowTitle() is the name a window opens with");

    // No sink: a change is accepted and counted, and reaches nothing (the cell grid).
    tui::setWindowTitleSink(nullptr);
    g_sinkTitles.clear();
    const unsigned before = tui::windowTitleChangeCount();
    tui::tuiBackendSetWindowTitle("grid session");
    check(tui::windowTitleChangeCount() == before + 1,
          "the seam accepts a change without a sink (the grid is a no-op, not a discard)");
    check(g_sinkTitles.empty(), "no sink is installed, so nothing was delivered");
    check(tui::lastWindowTitle() == "grid session",
          "the seam remembers the last title it forwarded");

    tui::setWindowTitleSink(&recordTitle);
    g_sinkTitles.clear();
    const unsigned installed = tui::windowTitleChangeCount();
    tui::tuiBackendSetWindowTitle("grid session");
    check(tui::windowTitleChangeCount() == installed + 1,
          "installing a sink forgets the record, so the next call forwards the current title");
    check(g_sinkTitles.size() == 1 && g_sinkTitles[0] == "grid session",
          "the sink received the title");

    const unsigned delivered = tui::windowTitleChangeCount();
    tui::tuiBackendSetWindowTitle("grid session");
    check(tui::windowTitleChangeCount() == delivered,
          "an unchanged title forwards nothing (a frame may call this every frame)");
    check(g_sinkTitles.size() == 1, "and delivers nothing");

    tui::tuiBackendSetWindowTitle("");
    check(tui::lastWindowTitle() == "llmfun", "an empty title is the application name");
    check(g_sinkTitles.size() == 2 && g_sinkTitles[1] == "llmfun",
          "the application name reached the sink");

    tui::setWindowTitleSink(nullptr);
    g_sinkTitles.clear();
}

// ---------------------------------------------------------------------------
// 8. The shell: one frame, the title mirror and the queued rename
// ---------------------------------------------------------------------------

/// One frame of the whole workbench shell, so the bar is exercised where it lives: the frame
/// derives its inputs from the aggregate and applies its answers.
views::ShellFrame shellFrame(tui::TuiState& state,
                             const std::function<void(ImGuiIO&)>& inject = nullptr) {
    views::ShellFrame out;
    frame(
        [&](const ui::Metrics&) {
            out = views::renderShell(
                state, ImVec2(static_cast<float>(kScreenW), static_cast<float>(kScreenH)));
        },
        inject);
    return out;
}

void checkShell() {
    tui::TuiState state;
    state.sessionPanel.sessions.push_back(tui::SessionEntry{"s1", "Demo session", "", 3, true});
    state.sessionPanel.activeId = "s1";
    tui::setWindowTitleSink(&recordTitle);
    g_sinkTitles.clear();

    const views::ShellFrame first = shellFrame(state);
    check(state.topBar.sessionId == "s1", "the bar reads the active session's id");
    check(state.topBar.sessionTitle == "Demo session", "the bar reads the active session's title");
    check(state.topBar.run == views::RunState::Idle, "an idle frame is the bar's idle state");
    check(!state.topBar.rawLogOpen, "the raw log starts closed");
    check(!g_sinkTitles.empty() && g_sinkTitles.back() == "Demo session",
          "the window title mirrors the session title");

    // A second frame with the same title forwards nothing more.
    const std::size_t delivered = g_sinkTitles.size();
    shellFrame(state);
    check(g_sinkTitles.size() == delivered, "an unchanged title is not mirrored again");

    // The run states come from what the frame owns: busy, a requested stop, and the offline flag.
    // The transition goes through the API D uses, because that is also where the run clock starts
    // (a direct `readyStatus` write would skip the edge and hide the bug it fixes).
    state.startProcesssingTime = std::chrono::system_clock::now() - std::chrono::minutes(5);
    tui::tuiSetReadyStatus(state, false); // D's `uiAgentBusy()`
    shellFrame(state);
    check(state.topBar.run == views::RunState::Running, "a busy frame is 'running'");
    check(state.topBar.elapsedMs < 2000,
          "the run clock starts at the run's edge, not at construction (" +
              std::to_string(state.topBar.elapsedMs) + " ms)");

    // A repeated "not ready" (a stream tick) must not restart the clock.
    state.startProcesssingTime = std::chrono::system_clock::now() - std::chrono::seconds(30);
    tui::tuiSetReadyStatus(state, false);
    shellFrame(state);
    check(state.topBar.elapsedMs >= 29000 && state.topBar.elapsedMs < 32000,
          "a repeated busy call keeps the run clock (" + std::to_string(state.topBar.elapsedMs) +
              " ms)");

    state.cancelRequested = true;
    shellFrame(state);
    check(state.topBar.run == views::RunState::Waiting, "a requested stop is 'waiting'");
    state.cancelRequested = false;
    tui::tuiSetReadyStatus(state, true);
    state.ui.offline = true;
    shellFrame(state);
    check(state.topBar.run == views::RunState::Failed, "an unreachable backend is 'failed'");
    state.ui.offline = false;
    shellFrame(state);

    // Rename through the bar: exactly one queued action, and it is the rail's own action shape.
    const views::TopBarParts parts =
        views::topBarMeasure(state.topBar, first.layout.topBar, ui::metricsFor(true, 1.0f, 1.0f));
    check(!parts.title.empty(), "the shell's bar has a title box to click");
    const float x = parts.title.x + 2.0f;
    const float y = parts.title.y;
    shellFrame(state, [&](ImGuiIO& io) { io.AddMousePosEvent(x, y); });
    shellFrame(state, [&](ImGuiIO& io) {
        io.AddMousePosEvent(x, y);
        io.AddMouseButtonEvent(0, true);
    });
    shellFrame(state, [&](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); });
    check(state.topBar.renaming, "a click on the shell's title opens the box");
    check(state.sessionPanel.actions.empty(), "opening the box queues nothing");

    shellFrame(state);
    for (const char c : std::string("Plan B"))
        shellFrame(state,
                   [&](ImGuiIO& io) { io.AddInputCharacter(static_cast<unsigned short>(c)); });
    shellFrame(state, [&](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Enter, true); });
    shellFrame(state, [&](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Enter, false); });

    check(state.sessionPanel.actions.size() == 1,
          "one commit queues exactly one action (" +
              std::to_string(state.sessionPanel.actions.size()) + ")");
    if (state.sessionPanel.actions.size() == 1) {
        const tui::SessionAction& action = state.sessionPanel.actions.front();
        check(action.type == tui::SessionActionType::Rename, "the queued action is a Rename");
        check(action.sessionId == "s1", "it renames the session the box was opened on");
        check(action.title == "Plan B", "it carries the edited title (" + action.title + ")");
    }
    check(!state.topBar.renameCommitted && state.topBar.renamedTitle.empty(),
          "the answer is consumed: it cannot be queued twice");
    check(!state.topBar.renaming, "the box is closed after the commit");

    // The bar is a projection of the snapshot: the committed title is *not* shown until D's next
    // list carries it (two frames after the commit at the actor's cadence), and the window mirror
    // follows the same string. A local copy would have to be reconciled against the snapshot and
    // would have no answer for a rename D refuses - so there is none.
    check(state.topBar.sessionTitle == "Demo session",
          "the bar still shows the snapshot's title while the rename is in flight (" +
              state.topBar.sessionTitle + ")");
    check(tui::lastWindowTitle() == "Demo session",
          "and so does the window (" + tui::lastWindowTitle() + ")");

    // D echoes the rename: the snapshot carries it, so that is what the bar draws.
    state.sessionPanel.sessions[0].title = "Plan B";
    shellFrame(state);
    check(state.topBar.sessionTitle == "Plan B", "the echoed title is the snapshot's now");
    check(tui::lastWindowTitle() == "Plan B",
          "and the window mirrors it (" + tui::lastWindowTitle() + ")");
    check(!g_sinkTitles.empty() && g_sinkTitles.back() == "Plan B",
          "the pixel backend's sink received it");

    // Two commits inside one round trip: both are requests, so both are queued, and the bar shows
    // the snapshot until the second one comes back. Nothing local holds the first title, so there
    // is nothing to reconcile and no order to get wrong.
    const auto renameTo = [&](const char* typed) {
        shellFrame(state, [&](ImGuiIO& io) { io.AddMousePosEvent(x, y); });
        shellFrame(state, [&](ImGuiIO& io) {
            io.AddMousePosEvent(x, y);
            io.AddMouseButtonEvent(0, true);
        });
        shellFrame(state, [&](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); });
        shellFrame(state);
        for (const char c : std::string(typed))
            shellFrame(state,
                       [&](ImGuiIO& io) { io.AddInputCharacter(static_cast<unsigned short>(c)); });
        shellFrame(state, [&](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Enter, true); });
        shellFrame(state, [&](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Enter, false); });
    };
    state.sessionPanel.actions.clear();
    renameTo("Plan C");
    check(state.topBar.sessionTitle == "Plan B",
          "the first of two commits does not move the title on its own (" +
              state.topBar.sessionTitle + ")");
    renameTo("Plan D");
    check(state.sessionPanel.actions.size() == 2,
          "both commits queue an action (" + std::to_string(state.sessionPanel.actions.size()) +
              ")");
    state.sessionPanel.sessions[0].title = "Plan D";
    shellFrame(state);
    check(state.topBar.sessionTitle == "Plan D", "the echoed second title is the snapshot's now");

    // An Esc a view-local handler already claimed is not the bar's (D-ESC: one step per press).
    // `escapeConsumed` is the frame's latch, set by `tui.cpp` before the shell draws; the press
    // itself still arrives at the bar, which is what makes the latch the only thing stopping it.
    state.topBar.renaming = true;
    state.topBar.renameId = "s1";
    state.topBar.renameFocus = false;
    state.topBar.renameHadFocus = false;
    state.escapeConsumed = true;
    shellFrame(state, [&](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Escape, true); });
    check(state.topBar.renaming, "an already-claimed Esc does not close the box");
    check(!state.topBar.escapeConsumed, "and the bar did not claim it a second time");
    shellFrame(state, [&](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Escape, false); });
    // With the latch clear, the same press is the bar's and its answer reaches the frame.
    state.escapeConsumed = false;
    shellFrame(state, [&](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Escape, true); });
    check(!state.topBar.renaming, "an unclaimed Esc closes the box");
    check(state.escapeConsumed, "and the bar's answer reaches the frame latch");
    check(!g_sinkTitles.empty() && g_sinkTitles.back() == "Plan D",
          "the window title stayed the committed one through the frames in between");

    // A run that starts while the box is open: the rename is refused and the draft stays. The
    // sequence is the reachable one - a busy bar does not open a box at all - and it runs through
    // the shell, so the field is the focused one the earlier renames proved.
    state.sessionPanel.actions.clear();
    state.escapeConsumed = false; // the Esc checks above left the frame latch set
    shellFrame(state, [&](ImGuiIO& io) { io.AddMousePosEvent(x, y); });
    shellFrame(state, [&](ImGuiIO& io) {
        io.AddMousePosEvent(x, y);
        io.AddMouseButtonEvent(0, true);
    });
    shellFrame(state, [&](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); });
    shellFrame(state);
    check(state.topBar.renaming, "the box is open for the busy case");
    for (const char c : std::string("Plan E"))
        shellFrame(state,
                   [&](ImGuiIO& io) { io.AddInputCharacter(static_cast<unsigned short>(c)); });
    state.readyStatus = false; // a run starts mid-edit (D's `uiAgentBusy()`)
    shellFrame(state);
    check(!state.topBar.canRename, "a run turns the rename off");
    shellFrame(state, [&](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Enter, true); });
    shellFrame(state, [&](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Enter, false); });
    check(state.sessionPanel.actions.empty(),
          "a rename is not queued while a run is active (" +
              std::to_string(state.sessionPanel.actions.size()) + ")");
    check(state.topBar.renaming, "and the box stays open with the draft");
    state.readyStatus = true;
    shellFrame(state);

    // The tooltip/disabled path is not a queue path either.
    state.sessionPanel.actions.clear();
    tui::setWindowTitleSink(nullptr);
}

// ---------------------------------------------------------------------------
// 9. Goldens
// ---------------------------------------------------------------------------

struct Scene {
    const char* name;
    views::RunState run;
    bool rawLogOpen;
    const char* title;
    int w, h;
};

std::vector<Scene> scenes() {
    const char* longTitle = "a session title that is much longer than one row can hold, twice over";
    return {
        {"topbar-idle", views::RunState::Idle, false, "Demo session", kScreenW, kScreenH},
        {"topbar-running", views::RunState::Running, false, "Demo session", kScreenW, kScreenH},
        {"topbar-waiting", views::RunState::Waiting, false, "Demo session", kScreenW, kScreenH},
        {"topbar-failed", views::RunState::Failed, false, "Demo session", kScreenW, kScreenH},
        {"topbar-rawlog", views::RunState::Running, true, "Demo session", kScreenW, kScreenH},
        {"topbar-narrow", views::RunState::Running, true, longTitle, 60, 20},
    };
}

std::string goldenText(const Scene& scene) {
    const ui::Metrics m = ui::metricsFor(true, 1.0f, 1.0f);
    const ui::Rect bar = shellTopBar(scene.w, scene.h, m);
    views::TopBarState state;
    state.sessionId = "s1";
    state.sessionTitle = scene.title != nullptr ? scene.title : "";
    state.run = scene.run;
    if (scene.run == views::RunState::Running || scene.run == views::RunState::Waiting)
        state.elapsedMs = 12000;
    state.rawLogOpen = scene.rawLogOpen;
    const views::TopBarParts parts = views::topBarMeasure(state, bar, m);

    Grid grid;
    {
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(static_cast<float>(scene.w), static_cast<float>(scene.h));
        io.DeltaTime = 1.0f / 60.0f;
        ImTui_ImplText_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(scene.w), static_cast<float>(scene.h)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("topbar-golden", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        views::renderTopBar(state, bar, m);
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::Render();
        ImTui_ImplText_RenderDrawData(ImGui::GetDrawData(), &g_screen);
        grid = capture();
    }

    std::string text = "name: ";
    text += scene.name;
    text += "\nscreen: ";
    text += std::to_string(scene.w) + "x" + std::to_string(scene.h);
    text += "\nbar: " + partText({bar.x, bar.y, bar.w, bar.h});
    // The measured parts: a golden pins the arithmetic as well as the pixels, so a part that
    // moved by one cell fails the run even when the glyphs still happen to fit.
    text += "\nstate: " + std::string(views::runStateName(views::barState(state)));
    text += "\nparts: mark=" + num(parts.mark.w) + " wordmark=" + num(parts.wordmark.w) +
            " title=" + num(parts.title.w) + " hint=" + num(parts.hint.w) +
            " pill=" + num(parts.pill.w) + " badge=" + num(parts.badge.w) +
            " gear=" + num(parts.gear.w) + " truncated=" + (parts.titleTruncated ? "1" : "0");
    text += "\ntext:\n";
    for (int y = 0; y < grid.ny; ++y) {
        std::string row = grid.rowText(y);
        while (!row.empty() && row.back() == ' ')
            row.pop_back();
        text += row;
        text += '\n';
    }
    // A text dump cannot show a fill, and a chip/pill *is* partly its fill: the painted
    // background is recorded as runs of equal colour index, one line per row that has any.
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

void checkGoldens(bool update) {
    const std::string dir = TEST_TOPBAR_GOLDEN_DIR;
    const std::vector<Scene> all = scenes();
    check(all.size() == 6,
          "one golden per state plus the narrow one (6 of " + std::to_string(all.size()) + ")");
    for (const Scene& scene : all) {
        const std::string path = dir + "/" + std::string(scene.name) + "-" +
                                 std::to_string(scene.w) + "x" + std::to_string(scene.h) + ".txt";
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
            check(true, std::string("the ") + scene.name + " golden matches");
            continue;
        }
        std::size_t line = 1;
        std::size_t at = 0;
        while (at < expected.size() && at < text.size() && expected[at] == text[at]) {
            line += expected[at] == '\n' ? 1 : 0;
            ++at;
        }
        check(false, std::string("the ") + scene.name +
                         " golden matches (first difference on line " + std::to_string(line) +
                         ": " + path + ")");
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
        else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0)
            usage(argv[0], "print every check with -v, rewrite the goldens with --update");
        else
            usage(argv[0], "unknown argument");
    }

    // One glyph set for every frame, whatever terminal runs this: `LLMFUN_ASCII=0` outranks TERM
    // and the locale in the detection order, so a golden cannot depend on the environment.
    ::setenv("LLMFUN_ASCII", "0", 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImTui_ImplText_Init();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(kScreenW), static_cast<float>(kScreenH));
    io.DeltaTime = 1.0f / 60.0f;

    // One warm-up frame: the atlas is built on the first frame, and a text measure before it
    // answers zero.
    frame([](const ui::Metrics&) {});

    checkSource();
    checkLabels();
    checkMeasureParts();
    checkSweepFor(ui::metricsFor(true, 1.0f, 1.0f), "grid");
    checkSweepFor(ui::metricsFor(false, 1.0f, 1.0f), "pixel");
    checkNarrowBar();
    checkContrast();
    checkRename();
    checkTitleSeam();
    checkShell();
    checkGoldens(update);

    ImTui_ImplText_Shutdown();
    ImGui::DestroyContext();

    std::printf("%s: %d check(s), %d failure(s)\n", g_failures == 0 ? "PASS" : "FAIL", g_checks,
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
