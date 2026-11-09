// test_rawlog.cpp
//
// The Raw Log surface (views/rawlog.h + model/rawlog_data.h): the opt-in developer stream,
// its row format, its filter and follower, its save and
// copy, and the wiring that makes it reachable from every documented access path.
//
// What this binary proves, in order:
//
//   1. the source. The surface writes no colour literal, reads no display size, names no v4 type
//      and no `tui.h`; the strings it prints are the header's, not literals in the .cpp;
//   2. the model. The row text (`time level module.function file:line message`), the detail
//      lines, the case-insensitive filter, the `UiLog` floor, the save name/path and the one
//      write - including "never overwrite", "never inside the workarea" and "the archive keeps
//      its own level" (the archive's level is never touched here);
//   3. the surface. Title, counts, chips (`follow`/`paused`), the dropped row, the empty and
//      no-match states, the bounded window (5000 records draw a viewport, not the list), the
//      expanded detail and the copy payload;
//   4. the follower. Content-driven and capped at 10 fps: a quiet stream pins nothing, a busy
//      one pins at most once per 100 ms, and a wheel notch clears the flag;
//   5. the frame wiring. Closed on every start, `/rawlog` intercepted (never submitted, never in
//      history), the `RAW` badge, the modal overlay's `Esc` step, the top of the overlay stack,
//      the TUI overlay covering the screen, and the GUI drawer never covering the composer;
//   6. the ABI. `tuiSetRawLogOpen`/`tuiIsRawLogOpen`, both null-safe, and the open flag the D
//      loop polls to raise and restore its logger level;
//   7. the goldens: follow, filtered, empty.
//
// Exit codes: 0 = all checks pass; 1 = a check failed; 2 = usage error.
//
//   ./test_rawlog [-v] [--update] [--print]
//
// `--update` rewrites the fixtures; do that only for a deliberate rendering change, and commit
// the diff with it. The captures pin `TZ=UTC`: a row's clock is local time, so a golden captured
// in another zone would differ for a reason that is not a defect.

#include "views/rawlog.h"

#include "views/chrome.h"
#include "views/shell.h"

#include "tui.h"

#include "tui_api.h"

#include "test/tui_state_handle.h"

#include "model/activity.h"
#include "model/settings.h"
#include "model/ui_state.h"
#include "ui/icons.h"
#include "ui/layout.h"
#include "ui/metrics.h"
#include "ui/text.h"

#include "imtui/imtui.h"

#include "imgui/imgui.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#include <unistd.h>

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

[[noreturn]] void usage(const char* argv0, const std::string& problem) {
    std::fprintf(stderr, "usage: %s [-v] [--update] [--print]   (%s)\n", argv0, problem.c_str());
    std::exit(2);
}

std::string readWhole(const std::string& path) {
    std::ifstream in(path);
    if (!in.good())
        return std::string();
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path);
    if (!out.good())
        return false;
    out << text;
    return out.good();
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

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
    std::vector<uint8_t> fg;

    uint32_t at(int x, int y) const {
        if (x < 0 || y < 0 || x >= nx || y >= ny)
            return 0;
        return ch[static_cast<std::size_t>(y) * nx + x];
    }

    /// The cell's foreground ink, as the backend encoded it: whatever palette
    /// entry the resolved color became this frame, never a hard-coded index.
    uint8_t fgAt(int x, int y) const {
        if (x < 0 || y < 0 || x >= nx || y >= ny)
            return 0;
        return fg[static_cast<std::size_t>(y) * nx + x];
    }

    std::string rowText(int y) const {
        std::string text;
        for (int x = 0; x < nx; ++x) {
            const uint32_t cp = at(x, y);
            text += cp == 0 ? std::string(" ") : utf8Of(cp);
        }
        return text;
    }

    std::string allText() const {
        std::string text;
        for (int y = 0; y < ny; ++y) {
            text += rowText(y);
            text += '\n';
        }
        return text;
    }

    bool hasText(const std::string& needle) const { return contains(allText(), needle); }

    int rowOf(const std::string& needle) const {
        for (int y = 0; y < ny; ++y) {
            if (contains(rowText(y), needle))
                return y;
        }
        return -1;
    }
};

Grid capture() {
    Grid grid;
    grid.nx = g_screen.nx;
    grid.ny = g_screen.ny;
    const std::size_t cells = static_cast<std::size_t>(grid.nx) * grid.ny;
    grid.ch.resize(cells);
    grid.fg.resize(cells);
    for (std::size_t i = 0; i < cells; ++i) {
        grid.ch[i] = g_screen.data[i].ch;
        grid.fg[i] = g_screen.data[i].fg;
    }
    return grid;
}

Grid frame(const std::function<void(const ui::Metrics&)>& body, const char* chars = nullptr) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(g_w), static_cast<float>(g_h));
    io.DeltaTime = 1.0f / 60.0f;
    ImTui_ImplText_NewFrame();
    ImGui::NewFrame();
    // Typed characters, queued the way a backend leaves them for the frame that draws: the surface
    // reads them from the character queue, not from key events (plan/key_gate.md F2), and the
    // queue is what `EndFrame` clears.
    for (const char* c = chars; c != nullptr && *c != '\0'; ++c)
        io.InputQueueCharacters.push_back(static_cast<ImWchar>(static_cast<unsigned char>(*c)));
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(g_w), static_cast<float>(g_h)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("rawlog-frame", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus);
    body(ui::metricsFor(true, 1.0f, 1.0f));
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::Render();
    ImTui_ImplText_RenderDrawData(ImGui::GetDrawData(), &g_screen);
    const Grid grid = capture();
    if (g_print) {
        std::printf("--- frame %dx%d\n", g_w, g_h);
        for (int y = 0; y < grid.ny; ++y)
            std::printf("%2d |%s|\n", y, grid.rowText(y).c_str());
    }
    return grid;
}

const ui::Metrics& gridMetrics() {
    static const ui::Metrics m = ui::metricsFor(true, 1.0f, 1.0f);
    return m;
}

/// Draws a probe glyph with the given ink on a screen cell so an ink
/// assertion can compare a drawn cell's fg against the same token rendered
/// by the normal text path. Must be called inside the same frame() body,
/// after the view, on a cell that carries no view glyph.
void probeToken(const ui::Metrics& m, int x, int y, ui::ColorToken token) {
    ImGui::GetWindowDrawList()->AddText(ImVec2(x * m.gu, y * m.row),
                                        ImGui::GetColorU32(ui::color(token)), "x");
}

void checkInkCapture() {
    const ui::Metrics& m = gridMetrics();
    g_w = 100;
    g_h = 30;
    // The cells the probe will paint into, empty before anything draws: a probe
    // that overwrote a view glyph would lie about whose ink it carries.
    const Grid blank = frame([](const ui::Metrics&) {});
    check(blank.at(4, 8) == 0 || blank.at(4, 8) == ' ', "premise: the probe cells are blank");
    check(blank.at(7, 8) == 0 || blank.at(7, 8) == ' ', "premise: the second probe cell is blank");

    const Grid probed = frame([&](const ui::Metrics& mm) {
        probeToken(mm, 4, 8, ui::ColorToken::textLow);
        probeToken(mm, 7, 8, ui::ColorToken::accent);
        probeToken(mm, 10, 8, ui::ColorToken::textLow);
    });
    check(probed.at(4, 8) == 'x', "premise: the probe drew its glyph");
    check(probed.fgAt(4, 8) != probed.fgAt(7, 8), "different tokens leave different ink");
    check(probed.fgAt(4, 8) == probed.fgAt(10, 8), "the same token leaves the same ink");
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

/// 2026-06-21T00:00:00Z: the goldens read 00:00:xx and cannot embed the wall clock.
inline constexpr std::int64_t kFixtureEpochMs = 1'782'000'000'000;

model::RawLogRecord record(int level, const std::string& message, int index = 0,
                           const std::string& detail = std::string()) {
    model::RawLogRecord out;
    out.timestampMs = kFixtureEpochMs + 1000LL * index;
    out.level = level;
    out.action = 0;
    out.line = 412;
    out.message = message;
    out.detail = detail;
    out.module = "llm.agent.pool";
    out.function = "processTurn";
    out.file = "pool.d";
    return out;
}

/// The long-location variant the location rule and the wide plan measure against: its
/// full form needs 54 cells, six past the narrow column's 36, so only the tests that
/// pin truncation and the wide stretch reach for it.
model::RawLogRecord longLocationRecord(int level, const std::string& message, int index = 0,
                                       const std::string& detail = std::string()) {
    model::RawLogRecord out = record(level, message, index, detail);
    out.file = "source/llm/agent/pool.d";
    return out;
}

std::deque<model::RawLogRecord> mixedRecords() {
    std::deque<model::RawLogRecord> records;
    records.push_back(record(model::kRawLogLevelInfo, "session opened", 0));
    records.push_back(record(model::kRawLogLevelTrace, "tool args resolved", 1,
                             "args: path=README.md\nargs: limit=200"));
    records.push_back(record(model::kRawLogLevelWarn, "ui.conf is read-only", 2));
    model::RawLogRecord failed = record(model::kRawLogLevelError, "read_file failed", 3);
    failed.action = TuiLogAction_Retry;
    failed.actionPayload = "turn-7";
    records.push_back(failed);
    return records;
}

model::RawLogRing ringOf(const std::deque<model::RawLogRecord>& records, std::size_t dropped = 0) {
    return model::RawLogRing{&records, dropped};
}

views::RawLogState openSurface(const ui::Rect& rect, const model::RawLogRing& ring) {
    views::RawLogState state;
    state.open = true;
    state.overlayShape = true;
    state.minLevel = model::kRawLogLevelTrace;
    // Two frames: the first measures the content, the second is what a capture sees (the
    // follower's pin reads the previous frame's ScrollMax, like every scrolled child).
    for (int i = 0; i < 2; ++i)
        frame([&](const ui::Metrics& mm) { views::renderRawLog(state, rect, mm, ring); });
    return state;
}

ui::Rect surfaceRect(int w, int h) {
    return ui::Rect{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), true};
}

// ---------------------------------------------------------------------------
// 1. The source
// ---------------------------------------------------------------------------

void checkSource() {
    const std::string cpp = readWhole(std::string(TEST_RAWLOG_VIEW_SOURCE));
    const std::string header = readWhole(std::string(TEST_RAWLOG_HEADER_SOURCE));
    check(!cpp.empty(), "the surface's source is readable");
    check(!contains(cpp, "#include \"tui.h\""), "views/rawlog.cpp includes no tui.h (D-ADAPTER)");
    check(!contains(cpp, "ChatMessage"), "and names no v4 message type");
    check(!contains(cpp, "RenderGroup"), "and no v4 render group");
    check(!contains(cpp, "displaySize()"), "and reads no display size (D-METRICS)");
    check(!contains(cpp, "ImVec4("), "and writes no colour literal (D-THEME)");
    check(!contains(cpp, "IsKeyPressed"), "and binds no global chord itself (D-KEYMAP)");
    check(!contains(header, "#include \"tui.h\""), "views/rawlog.h includes no tui.h either");
    // The vocabulary lives in the header: the .cpp prints it, never a literal of its own.
    check(!contains(cpp, "\"Raw Log\""), "the title is the header's string, not a literal");
    check(!contains(cpp, "\"No records yet\""), "and so is the empty state");
    // The save path is built outside the workarea: the model never uses the CWD.
    const std::string data = readWhole(std::string(TEST_RAWLOG_MODEL_SOURCE));
    check(!data.empty(), "the model's source is readable");
    check(!contains(data, "getcwd"), "the save path is never built from the working directory");
    check(!contains(data, "tuiSetLogging"), "and the surface never turns the archive on (R-08)");
}

// ---------------------------------------------------------------------------
// 2. The model
// ---------------------------------------------------------------------------

void checkRowText() {
    model::RawLogRecord full = longLocationRecord(model::kRawLogLevelWarn, "ui.conf is read-only");
    const std::string row = model::rawLogRowText(full);
    check(contains(row, "00:00:00.000"), "the row starts with the millisecond clock");
    check(contains(row, " warn  "), "the level is a padded five-cell field");
    check(contains(row, "llm.agent.pool.processTurn"),
          "the module and function are joined by a dot");
    check(contains(row, "source/llm/agent/pool.d:412"), "the file carries its line");
    check(row.size() > 20 && row.compare(row.size() - 20, 20, "ui.conf is read-only") == 0,
          "and the message is last");

    model::RawLogRecord plain;
    plain.timestampMs = kFixtureEpochMs;
    plain.level = model::kRawLogLevelInfo;
    plain.message = "bare";
    const std::string bare = model::rawLogRowText(plain);
    check(contains(bare, "info  bare"), "a record with no metadata prints no empty separators");

    model::RawLogRecord unstamped;
    unstamped.level = model::kRawLogLevelInfo;
    unstamped.message = "no clock";
    check(contains(model::rawLogRowText(unstamped), "--:--:--.---"),
          "a record without a stamp prints dashes, not the epoch");

    check(std::string(model::rawLogLevelWord(model::kRawLogLevelWarn)) == "warn",
          "the severity word is `warn`, the archive's and Activity's spelling");
    check(std::string(model::rawLogLevelWord(model::kRawLogLevelTrace)) == "trace",
          "and `trace` for the developer step");
    check(std::string(model::rawLogLevelWord(99)) == "info", "an unknown level reads as info");
}

// ---------------------------------------------------------------------------
// 1b. The column plan
// ---------------------------------------------------------------------------

void checkColumnPlan() {
    const ui::Metrics& m = gridMetrics();
    const float gu = m.gu;
    const auto wholeCells = [&](float v) { return std::fmod(v, gu) == 0.0f; };

    // The rung table's boundaries, pinned one cell past each edge.
    const struct {
        int cells;
        int rung;
        float msgXCells;
    } boundaries[] = {
        {9, 4, 10},  {10, 4, 10}, {31, 4, 10}, {32, 3, 20}, {69, 3, 20},  {70, 2, 58},
        {73, 2, 58}, {74, 1, 62}, {83, 1, 62}, {84, 0, 62}, {100, 0, 62},
    };
    for (const auto& b : boundaries) {
        const float bodyW = static_cast<float>(b.cells) * gu;
        const views::RowPlan plan = views::rawRowPlan(bodyW, m);
        const std::string at = std::to_string(b.cells) + " cells";
        check(plan.rung == b.rung,
              "the ladder answers rung " + std::to_string(b.rung) + " at " + at);
        check(plan.msgX == b.msgXCells * gu, "the message column starts at cell " +
                                                 std::to_string(static_cast<int>(b.msgXCells)) +
                                                 " at " + at);
        check(plan.markerX == 0.0f && plan.markerW == gu,
              "the marker is the row's one reserved first cell at " + at);
        check(plan.levelW == 5.0f * gu, "the level field keeps its five cells at " + at);
        check(plan.msgW == std::max(0.0f, bodyW - plan.msgX),
              "the message takes the rest of the row at " + at);
        check(wholeCells(plan.markerW) && wholeCells(plan.clockW) && wholeCells(plan.levelW) &&
                  wholeCells(plan.locW) && wholeCells(plan.msgX) && wholeCells(plan.tagW),
              "every planned width is whole cells at " + at);
        if (plan.rung == 0) {
            check(plan.clockX == 3.0f * gu && plan.clockW == 12.0f * gu &&
                      plan.levelX == 17.0f * gu && plan.locX == 24.0f * gu &&
                      plan.locW == 36.0f * gu && plan.tagW == 10.0f * gu,
                  "the widest rung's segment offsets hold at " + at);
        } else if (plan.rung == 1) {
            check(plan.clockX == 3.0f * gu && plan.clockW == 12.0f * gu &&
                      plan.levelX == 17.0f * gu && plan.locX == 24.0f * gu &&
                      plan.locW == 36.0f * gu && plan.tagW == 0.0f,
                  "the tagless wide rung's segment offsets hold at " + at);
        } else if (plan.rung == 2) {
            check(plan.clockX == 3.0f * gu && plan.clockW == 8.0f * gu &&
                      plan.levelX == 13.0f * gu && plan.locX == 20.0f * gu &&
                      plan.locW == 36.0f * gu && plan.tagW == 0.0f,
                  "the short-clock rung's offsets hold at " + at);
        } else if (plan.rung == 3) {
            check(plan.clockX == 3.0f * gu && plan.clockW == 8.0f * gu &&
                      plan.levelX == 13.0f * gu && plan.locW == 0.0f && plan.tagW == 0.0f,
                  "the no-location rung's offsets hold at " + at);
        } else {
            check(plan.clockW == 0.0f && plan.locW == 0.0f && plan.tagW == 0.0f &&
                      plan.levelX == 3.0f * gu,
                  "the narrowest rung keeps the marker and the level alone at " + at);
        }
    }

    // Degenerate frames plan, never divide by zero or underflow.
    for (const float bodyW : {0.0f, -1.0f}) {
        const views::RowPlan plan = views::rawRowPlan(bodyW, m);
        check(plan.rung == 4 && plan.msgW == 0.0f,
              "a degenerate body width plans rung 4 with no message");
    }

    // The location rule: widest form first, whole pieces, never a fragment.
    const model::RawLogRecord located = longLocationRecord(model::kRawLogLevelInfo, "warming up");
    check(views::rawLogLocationText(located, 54) ==
              "llm.agent.pool.processTurn source/llm/agent/pool.d:412",
          "the location's full form when the column fits it");
    check(views::rawLogLocationText(located, 36) == "agent.pool.processTurn pool.d:412",
          "one trim pass shortens the module and basenames the file");
    const std::string narrow = views::rawLogLocationText(located, 20);
    check(contains(narrow, "...") && contains(narrow, "pool.d:412") &&
              ui::displayWidth(narrow) == 20,
          "the mid-ellipsis keeps the name's tail and the whole file part");
    check(views::rawLogLocationText(located, 8).empty(),
          "a column too narrow even for the ellipsis's minimum shows no location");
    model::RawLogRecord plain;
    plain.message = "bare";
    check(views::rawLogLocationText(plain, 36).empty(),
          "a record with no coordinates shows no location");
    for (int cells = 0; cells <= 60; ++cells) {
        const std::string text = views::rawLogLocationText(located, cells);
        check(text.empty() || ui::displayWidth(text) <= cells,
              "the location rule never returns a fragment (" + std::to_string(cells) + " cells)");
        check(text.empty() || contains(text, "pool.d:412"),
              "and it never cuts the file part away (" + std::to_string(cells) + " cells)");
    }
}

// The wide plan and the measured maxima

void checkWidePlanAndMaxima() {
    const ui::Metrics m = gridMetrics();
    const float gu = m.gu;

    // A degenerate measurement answers the narrow plan: nothing measured, nothing to widen.
    const views::RowPlan narrow = views::rawRowPlan(100.0f * gu, m);
    const views::RowPlan degenerate = views::rawRowPlanWide(100.0f * gu, m, 54.0f * gu, 0.0f);
    check(degenerate.rung == narrow.rung && degenerate.msgX == narrow.msgX &&
              degenerate.msgW == narrow.msgW && degenerate.locW == narrow.locW &&
              degenerate.tagW == narrow.tagW,
          "a wide plan without a measurement answers the narrow plan's shape");

    // The measured widest location widens the column; the message loses its ellipsis and the
    // tag its column. The leading rungs and the location's start stay put.
    const views::RowPlan wide = views::rawRowPlanWide(100.0f * gu, m, 54.0f * gu, 200.0f * gu);
    check(wide.rung == narrow.rung, "the wide plan keeps the narrow plan's rung");
    check(wide.locW == 54.0f * gu, "the location column widens to the measured widest");
    check(wide.locX == narrow.locX, "and the location column keeps its narrow start");
    check(wide.msgX == wide.locX + wide.locW + 2.0f * gu,
          "the message starts one gap past the widened location column");
    check(wide.msgW == 200.0f * gu, "the message draws at its full natural width");
    check(wide.tagW == 0.0f, "and the tag is dropped whole in wide mode");
    check(wide.clockX == narrow.clockX && wide.clockW == narrow.clockW &&
              wide.levelX == narrow.levelX && wide.levelW == narrow.levelW,
          "the leading rungs keep their narrow geometry");

    // The cap: one pathological location cannot own the surface.
    const views::RowPlan capped = views::rawRowPlanWide(100.0f * gu, m, 500.0f * gu, 200.0f * gu);
    check(capped.locW == 72.0f * gu, "the location column clamps at kRawLogMaxLocationCells");

    // A rung that dropped the location keeps it dropped: the wide variant widens a
    // column the ladder still carries, never one the rung gave up.
    const views::RowPlan rung3 = views::rawRowPlanWide(69.0f * gu, m, 54.0f * gu, 40.0f * gu);
    check(rung3.rung == 3 && rung3.locW == 0.0f &&
              rung3.msgX == views::rawRowPlan(69.0f * gu, m).msgX,
          "the wide plan never resurrects a location the rung dropped");

    // The content width: the message's start, the widest drawn extent and one trailing gap.
    check(views::rawLogContentW(wide, 200.0f * gu, m) == wide.msgX + 200.0f * gu + 2.0f * gu,
          "the content width is the message start plus the widest extent plus a gap");
    check(views::rawLogContentW(wide, 200.0f * gu, m) > 100.0f * gu,
          "premise: this content overflows the body, so wide mode engages");

    // The walk measures what the rows draw, so the maxima are the surface's own answer. The
    // record's full location is 54 cells (the unit the tests above pin) - six past the
    // narrow column, so the walk's untruncated number is the interesting one.
    const ui::Rect rect = surfaceRect(100, 30);
    const std::deque<model::RawLogRecord> records{
        longLocationRecord(model::kRawLogLevelInfo, std::string(200, 'x'), 0)};
    views::RawLogState state = openSurface(rect, ringOf(records));
    check(state.builtGu == gu, "the rebuild key carries the profile's font identity");
    check(state.maxMessageW == 200.0f, "the walk measured the widest message");
    check(state.maxLocationW == 54.0f, "and the widest location, untruncated");

    // The expanded record's detail lines join the measurement through the expansion.
    const std::deque<model::RawLogRecord> withDetail{
        record(model::kRawLogLevelInfo, "short", 0, "args: path=README.md\nargs: limit=200")};
    views::RawLogState expanded = openSurface(rect, ringOf(withDetail));
    expanded.expandedKey = views::rawLogKey(withDetail[0]);
    frame([&](const ui::Metrics& mm) {
        views::renderRawLog(expanded, rect, mm, ringOf(withDetail));
    });
    float widestDetail = 0.0f;
    for (const std::string& line : model::rawLogDetailLines(withDetail[0]))
        widestDetail = std::max(widestDetail, static_cast<float>(ui::displayWidth(line)));
    check(expanded.maxDetailW == widestDetail,
          "the expanded record's widest detail line is measured");
    check(expanded.maxMessageW == static_cast<float>(ui::displayWidth("short")),
          "and the message maximum is the message's own width");

    // An append re-runs the walk: the arrival counter changes on every arrival,
    // so the appended record's wider message becomes the maximum.
    std::deque<model::RawLogRecord> appended = records;
    appended.push_back(record(model::kRawLogLevelInfo, std::string(260, 'y'), 1));
    frame([&](const ui::Metrics& mm) { views::renderRawLog(state, rect, mm, ringOf(appended)); });
    check(state.maxMessageW == 260.0f,
          "an append re-measures, so the wider message becomes the maximum");

    // A filter that hides the widest row makes the maximum drop: a hidden
    // record can never own a column.
    frame([&](const ui::Metrics& mm) {
        std::snprintf(state.filter.data(), state.filter.size(), "%s", "x");
        ++state.filterSeq;
        views::renderRawLog(state, rect, mm, ringOf(appended));
    });
    check(state.maxMessageW == 200.0f,
          "a filter that hides the widest row drops the maximum to the kept rows' widest");
    std::snprintf(state.filter.data(), state.filter.size(), "%s", "");
    ++state.filterSeq;

    // A font-scale change re-runs the walk: the key carries the identity, so the maxima are
    // never trusted across a profile whose unit changed.
    const ui::Metrics scaled = ui::metricsFor(true, 2.0f, 1.0f);
    frame([&](const ui::Metrics&) { views::renderRawLog(state, rect, scaled, ringOf(records)); });
    check(state.builtGu == scaled.gu, "a font-scale change re-runs the measurement walk");
}

void checkDetailAndFilter() {
    model::RawLogRecord withDetail =
        record(model::kRawLogLevelTrace, "tool call", 0, "args: path=README.md\nargs: limit=200\n");
    const std::vector<std::string> lines = model::rawLogDetailLines(withDetail);
    check(lines.size() == 2, "the detail's trailing newline adds no empty continuation");
    check(lines.size() == 2 && contains(lines[0], "path=README.md"),
          "and each line is drawn with the continuation mark");

    model::RawLogRecord noDetail = record(model::kRawLogLevelInfo, "plain", 0);
    check(model::rawLogDetailLines(noDetail).empty(), "a record with no detail answers no lines");

    model::RawLogRecord actionOnly = record(model::kRawLogLevelError, "failed", 0);
    actionOnly.action = TuiLogAction_Retry;
    actionOnly.actionPayload = "turn-7";
    const std::vector<std::string> actionLines = model::rawLogDetailLines(actionOnly);
    check(actionLines.size() == 1 && contains(actionLines[0], "action 1 turn-7"),
          "an action-only record exposes the id/payload by expanding");

    check(model::rawLogMatches(withDetail, ""), "an empty filter matches everything");
    check(model::rawLogMatches(withDetail, "   "), "and so does a blanks-only one");
    check(model::rawLogMatches(withDetail, "readme"), "the filter is case-insensitive");
    check(model::rawLogMatches(withDetail, "POOL.D"), "and matches the file name");
    check(model::rawLogMatches(withDetail, "limit=200"),
          "and the detail, which is what a search for an argument is looking for");
    check(!model::rawLogMatches(withDetail, "nonexistent"), "a miss is a miss");

    // The pref IS a severity - the ladder's words name the ABI's severities directly, so the
    // floor compare (`level >= minLevel`) needs no mapping and the table below is the one
    // spellbook.
    check(model::kRawLogError == model::kRawLogLevelError &&
              model::kRawLogWarn == model::kRawLogLevelWarn &&
              model::kRawLogInfo == model::kRawLogLevelInfo &&
              model::kRawLogTrace == model::kRawLogLevelTrace,
          "the UiLog constants are the ABI's severities (settings.h static_asserts them too)");
    check(model::kUiLogLadder[0].severity == model::kRawLogError &&
              std::string(model::kUiLogLadder[0].word) == "error" &&
              model::kUiLogLadder[1].severity == model::kRawLogWarn &&
              std::string(model::kUiLogLadder[1].word) == "warn" &&
              model::kUiLogLadder[2].severity == model::kRawLogInfo &&
              std::string(model::kUiLogLadder[2].word) == "info" &&
              model::kUiLogLadder[3].severity == model::kRawLogTrace &&
              std::string(model::kUiLogLadder[3].word) == "trace",
          "the ladder table spells the four words in the sheet's cycle order");
    check(model::rawLogShowsLevel(model::kRawLogLevelTrace, model::kRawLogLevelInfo) == false,
          "a trace line is below the ordinary floor");
    check(model::rawLogShowsLevel(model::kRawLogLevelError, model::kRawLogLevelInfo),
          "an error always passes it");
}

std::string scratchRoot();

void checkN2Identity() {
    // A wrapped ring with a stamped front, so the head placeholder row is up
    // beside real rows; one record expanded, so detail lines sit inside the
    // window; a filter active, so the mapped list is narrower than the ring.
    std::deque<model::RawLogRecord> records;
    for (int i = 0; i < 4; ++i)
        records.push_back(record(model::kRawLogLevelInfo, "kept " + std::to_string(i), i));
    records[0].seq = 5;
    records[1].detail = "args: path=README.md\nargs: limit=200";
    records[1].seq = 8;
    records[2].action = TuiLogAction_Retry;
    records[2].actionPayload = "turn-7";
    records[3].seq = 10;
    const model::RawLogRing ring{&records, 4};
    check(model::rawLogHeadGap(records, 4) == 4, "premise: the ring's head gap is 4");

    g_w = 100;
    g_h = 30;
    const ui::Rect rect = surfaceRect(g_w, g_h);
    views::RawLogState state;
    state.open = true;
    state.overlayShape = true;
    state.minLevel = model::kRawLogLevelTrace;
    std::snprintf(state.filter.data(), state.filter.size(), "%s", "kept");
    ++state.filterSeq;
    for (int i = 0; i < 2; ++i)
        frame([&](const ui::Metrics& mm) { views::renderRawLog(state, rect, mm, ring); });
    state.expandedKey = views::rawLogKey(records[1]);
    frame([&](const ui::Metrics& mm) { views::renderRawLog(state, rect, mm, ring); });
    check(state.matchingRecords == records.size(), "premise: the filter keeps all four records");
    check(state.expandedLines == model::rawLogDetailLines(records[1]),
          "premise: the expanded record carries its detail lines");

    // The canonical texts, in the order the window shows them: the head row, each
    // mapped record's model row, the expanded record's detail lines after it. The
    // save file spells the same rows without the head row and without expansion.
    std::string copyExpected =
        std::to_string(model::rawLogHeadGap(records, 4)) + views::kRawLogHeadGapSuffix + "\n";
    std::string saveExpected;
    for (std::size_t i = 0; i < records.size(); ++i) {
        copyExpected += model::rawLogRowText(records[i]) + "\n";
        saveExpected += model::rawLogRowText(records[i]) + "\n";
        const std::vector<std::string> lines = model::rawLogDetailLines(records[i]);
        for (const std::string& line : lines) {
            saveExpected += line + "\n";
            if (i == 1)
                copyExpected += line + "\n";
        }
    }
    const std::size_t totalLines = 1 + state.matching.size() + state.expandedLines.size();
    check(views::rawLogVisibleText(state, ring, 0, totalLines) == copyExpected,
          "the copy path emits the model's canonical texts, byte for byte");

    const std::string root = scratchRoot();
    check(!root.empty(), "premise: the scratch data directory exists");
    setenv("XDG_DATA_HOME", root.c_str(), 1);
    std::string path;
    std::string error;
    check(model::rawLogSave(ring, "kept", kFixtureEpochMs, path, error), "the save writes a file");
    check(readWhole(path) == saveExpected, "and the save path emits the same canonical texts");
    unsetenv("XDG_DATA_HOME");
}

/// A directory that the save test owns: `$XDG_DATA_HOME` points at it, so nothing is written to
/// the real home and the "outside the workarea" rule is provable by construction.
std::string scratchRoot() {
    const std::string path = "/tmp/llmfun_rawlog_test";
    std::string cmd = "rm -rf " + path + " && mkdir -p " + path;
    if (std::system(cmd.c_str()) != 0)
        return std::string();
    return path;
}

/// The openability marker: a summary row whose record carries multi-line detail leads with a
/// low-ink `+` in the row's reserved first cell, and every record row reserves that cell
/// whether or not it carries the glyph - the message column stays put from row to row.
/// Draw-only: the copy payload, the save file and the filter haystack are spelled without it.
void checkOpenableGlyph() {
    // The marker leads the row, so a row's first non-blank cell says whether it reserved
    // the cell for it.
    g_w = 100;
    g_h = 30;
    const ui::Rect rect = surfaceRect(g_w, g_h);
    const auto firstMark = [](const Grid& grid, const char* needle) {
        const std::string row = grid.rowText(grid.rowOf(needle));
        std::size_t begin = 0;
        while (begin < row.size() && row[begin] == ' ')
            ++begin;
        return begin == row.size() ? ' ' : row[begin];
    };

    const std::deque<model::RawLogRecord> records = mixedRecords();
    views::RawLogState state = openSurface(rect, ringOf(records));
    const Grid grid = capture();
    check(firstMark(grid, "tool args resolved") == views::kRawLogOpenableGlyph[0],
          "the record with detail starts with the openability marker");
    check(firstMark(grid, "session opened") != views::kRawLogOpenableGlyph[0],
          "a detail-free row prints no marker in its reserved first cell");
    check(firstMark(grid, "ui.conf is read-only") != views::kRawLogOpenableGlyph[0],
          "and so does every other detail-free row");

    // The long message fills the message column, and the column is the same in every row:
    // the reservation comes out of the row's head, not out of the message, so the counts
    // and the start cell are identical with and without the marker.
    model::RawLogRecord withDetail =
        record(model::kRawLogLevelInfo, std::string(80, 'x'), 0, "args: path=README.md");
    model::RawLogRecord withoutDetail = withDetail;
    withoutDetail.detail.clear();
    const auto xs = [](const Grid& grid, const char* needle) {
        const std::string row = grid.rowText(grid.rowOf(needle));
        int count = 0;
        for (const char c : row)
            count += c == 'x' ? 1 : 0;
        return count;
    };
    const std::deque<model::RawLogRecord> detailOnly{withDetail};
    views::RawLogState detailed = openSurface(rect, ringOf(detailOnly));
    const Grid detailedGrid = capture();
    check(xs(detailedGrid, "00:00:00.000") < 80,
          "the long message is cut before the marker's cell");
    check(firstMark(detailedGrid, "00:00:00.000") == views::kRawLogOpenableGlyph[0],
          "with the marker still leading the row");
    const std::deque<model::RawLogRecord> bareOnly{withoutDetail};
    views::RawLogState bare = openSurface(rect, ringOf(bareOnly));
    const Grid bareGrid = capture();
    check(xs(bareGrid, "00:00:00.000") == xs(detailedGrid, "00:00:00.000"),
          "the message column draws the same cells with and without the marker");

    // The marker is an affordance, never content: the copy payload spells the model row whole,
    // and the filter does not see the glyph either.
    views::RawLogState copied = openSurface(rect, ringOf(detailOnly));
    copied.firstVisible = 0;
    copied.windowRows = 1;
    const std::string payload = views::rawLogVisibleText(copied, ringOf(detailOnly), 0, 1);
    check(contains(payload, model::rawLogRowText(withDetail)),
          "the copy carries the whole model row, truncated nowhere");
    views::RawLogState filtered = openSurface(rect, ringOf(detailOnly));
    std::snprintf(filtered.filter.data(), filtered.filter.size(), "%s",
                  views::kRawLogOpenableGlyph);
    frame(
        [&](const ui::Metrics& m) { views::renderRawLog(filtered, rect, m, ringOf(detailOnly)); });
    check(filtered.matchingRecords == 0, "the filter haystack has no glyph in it");

    // An expanded record flips its marker to the open glyph: openability is a fact of
    // the record, not of the view state, but *open* is view state -
    // `+` openable, `-` open, blank nothing to open.
    views::RawLogState expanded = openSurface(rect, ringOf(records));
    expanded.expandedKey = views::rawLogKey(records[1]);
    const Grid expandedGrid = frame(
        [&](const ui::Metrics& m) { views::renderRawLog(expanded, rect, m, ringOf(records)); });
    check(expandedGrid.hasText("path=README.md"), "premise: the record is expanded");
    check(firstMark(expandedGrid, "tool args resolved") == views::kRawLogOpenGlyph[0],
          "and its summary row carries the open glyph while expanded");

    // The continuation lines align under the message column, in the metadata ink.
    const auto contColOf = [&expandedGrid](int y, const char* needle) {
        const std::size_t at = expandedGrid.rowText(y).find(needle);
        return at == std::string::npos ? -1 : static_cast<int>(at);
    };
    const int detailRowY = expandedGrid.rowOf("tool args resolved");
    check(detailRowY > 0, "premise: the expanded record's row is on screen");
    const int contY = detailRowY + 1;
    const int msgCol = contColOf(detailRowY, "tool args resolved");
    check(msgCol > 0, "premise: the summary row has a message column");
    // The model prefixes each detail line with "  | ", so the first word rides four
    // cells into the message column.
    check(contColOf(contY, "args:") == msgCol + 4,
          "the continuation line starts at the message column");
    const int probeY = contY + 4;
    check(expandedGrid.at(4, probeY) == ' ' && expandedGrid.at(6, probeY) == ' ',
          "premise: the probe row is blank");
    const Grid inkGrid = frame([&](const ui::Metrics& m) {
        views::renderRawLog(expanded, rect, m, ringOf(records));
        probeToken(m, 4, probeY, ui::ColorToken::textLow);
    });
    check(inkGrid.fgAt(msgCol + 4, contY) == inkGrid.fgAt(4, probeY),
          "the continuation line carries the metadata ink");
}

/// The record row's inks, pinned against same-frame probes: the clock, the location and
/// the reserved first cell carry the low ink, the level word and the message carry the
/// level's severity ink, and the action tag rides the right edge in the accent ink.
void checkRowInk() {
    g_w = 100;
    g_h = 30;
    const ui::Rect rect = surfaceRect(g_w, g_h);
    const std::deque<model::RawLogRecord> records = mixedRecords();
    const model::RawLogRing ring = ringOf(records);
    views::RawLogState state = openSurface(rect, ring);
    const Grid blankCheck =
        frame([&](const ui::Metrics& m) { views::renderRawLog(state, rect, m, ring); });
    const int errorY = blankCheck.rowOf("read_file failed");
    check(errorY > 0, "premise: the tagged error row is on screen");
    const int probeY = errorY + 2;
    check(blankCheck.at(4, probeY) == ' ', "premise: the probe row is blank");
    check(blankCheck.at(6, probeY) == ' ', "premise: the probe row is blank");
    check(blankCheck.at(8, probeY) == ' ', "premise: the probe row is blank");
    check(blankCheck.at(10, probeY) == ' ', "premise: the probe row is blank");

    const Grid grid = frame([&](const ui::Metrics& m) {
        views::renderRawLog(state, rect, m, ring);
        probeToken(m, 4, probeY, ui::ColorToken::textLow);
        probeToken(m, 6, probeY, ui::ColorToken::stateWarn);
        probeToken(m, 8, probeY, ui::ColorToken::stateDanger);
        probeToken(m, 10, probeY, ui::ColorToken::accent);
    });
    const auto colOf = [&grid](int y, const char* needle) {
        const std::size_t at = grid.rowText(y).find(needle);
        return at == std::string::npos ? -1 : static_cast<int>(at);
    };
    const int warn = blankCheck.rowOf("ui.conf is read-only");
    check(warn > 0, "premise: the warn row is on screen");
    check(grid.rowOf("00:00:02.000") == warn, "premise: the warn row spells its own clock");
    const int clockCol = colOf(warn, "00:00:02.000");
    check(clockCol >= 0, "premise: the clock is drawn whole");
    const int levelCol = colOf(warn, "warn ");
    check(levelCol == clockCol + 14, "the level word follows the clock by a two-cell gap");
    const int locCol = colOf(warn, "agent.pool.processTurn pool.d:412");
    check(locCol == levelCol + 7, "the location follows the level by a two-cell gap");
    const int msgCol = colOf(warn, "ui.conf is read-only");
    check(msgCol == locCol + 38, "the message follows the location by a two-cell gap");

    check(grid.fgAt(clockCol, warn) == grid.fgAt(4, probeY), "the clock carries the low ink");
    check(grid.fgAt(levelCol, warn) == grid.fgAt(6, probeY),
          "the level word carries the severity ink");
    check(grid.fgAt(locCol, warn) == grid.fgAt(4, probeY), "the location carries the low ink");
    check(grid.fgAt(msgCol, warn) == grid.fgAt(6, probeY),
          "the message carries the level's severity ink");
    const int taggedMsg = colOf(errorY, "read_file failed");
    check(taggedMsg == msgCol, "the message column is the same in every record row");
    check(grid.fgAt(taggedMsg, errorY) == grid.fgAt(8, probeY),
          "an error row's message carries the danger ink");
    const std::string tag = "action 1 turn-7";
    const int tagCol = colOf(errorY, tag.c_str());
    check(tagCol > taggedMsg, "premise: the action tag is drawn");
    check(grid.fgAt(tagCol, errorY) == grid.fgAt(10, probeY), "the tag carries the accent ink");
    const std::string errorRow = grid.rowText(errorY);
    int last = static_cast<int>(errorRow.size()) - 1;
    while (last >= 0 && errorRow[last] == ' ')
        --last;
    check(tagCol + static_cast<int>(tag.size()) == last + 1, "the tag rides the body's right edge");

    // The reserved first cell: a row with detail shows the marker there, and its message
    // still starts at the plan's column.
    const int detailY = grid.rowOf("tool args resolved");
    check(detailY > 0, "premise: the detail row is on screen");
    check(grid.at(msgCol - 62, detailY) == '+',
          "the openability marker sits in the row's first cell");
    check(colOf(detailY, "tool args resolved") == msgCol,
          "and the detail row's message starts at the same column");
}

/// A closed record with detail shows its first detail line as a preview
/// under its own row - at the message column, in the low ink, with no marker - and
/// the preview takes a body line in the scroll account while copying nothing.
void checkPreview() {
    g_w = 100;
    g_h = 30;
    const ui::Rect rect = surfaceRect(g_w, g_h);
    const std::deque<model::RawLogRecord> records = mixedRecords();
    const model::RawLogRing ring = ringOf(records);
    views::RawLogState state = openSurface(rect, ring);
    const Grid blank =
        frame([&](const ui::Metrics& m) { views::renderRawLog(state, rect, m, ring); });
    const int recordY = blank.rowOf("tool args resolved");
    check(recordY > 0, "premise: the trace record is on screen");
    const int previewY = recordY + 1;
    check(blank.rowOf("args: path=README.md") == previewY,
          "the preview line is the row under the record with detail");
    check(blank.at(2, previewY) == ' ', "the preview row reserves no marker cell");
    const auto colOf = [&blank](int y, const char* needle) {
        const std::size_t at = blank.rowText(y).find(needle);
        return at == std::string::npos ? -1 : static_cast<int>(at);
    };
    const int msgCol = colOf(recordY, "tool args resolved");
    check(msgCol > 0, "premise: the record row has a message column");
    // The preview text is the model's detail line verbatim, and the model prefixes
    // each detail line with "  | " - so the first detail word lands four cells into
    // the message column, its prefix riding those cells.
    check(colOf(previewY, "args:") == msgCol + 4,
          "the preview starts at the message column, prefix and all");

    // The preview's ink, pinned against same-frame probes: low like the auxiliary
    // text, not the record row's severity ink.
    const int probeY = previewY + 6;
    check(blank.at(4, probeY) == ' ' && blank.at(6, probeY) == ' ',
          "premise: the probe row is blank");
    const Grid ink = frame([&](const ui::Metrics& m) {
        views::renderRawLog(state, rect, m, ring);
        probeToken(m, 4, probeY, ui::ColorToken::textLow);
        probeToken(m, 6, probeY, ui::ColorToken::textMid);
    });
    check(ink.fgAt(msgCol + 4, previewY) == ink.fgAt(4, probeY), "the preview carries the low ink");
    check(ink.fgAt(msgCol + 4, previewY) != ink.fgAt(6, probeY),
          "and not the record row's message ink");

    // A record without detail shows no preview at all.
    const std::deque<model::RawLogRecord> bare{
        record(model::kRawLogLevelError, "read_file failed", 0)};
    views::RawLogState bareState = openSurface(rect, ringOf(bare));
    check(bareState.drawnRows == 1, "a record without detail draws no preview line");

    // Expanding the record replaces the preview with its continuation lines.
    views::RawLogState expanded = openSurface(rect, ring);
    expanded.expandedKey = views::rawLogKey(records[1]);
    const Grid expandedGrid =
        frame([&](const ui::Metrics& m) { views::renderRawLog(expanded, rect, m, ring); });
    check(expandedGrid.hasText("limit=200"), "premise: the expansion is drawn");
    check(expandedGrid.rowOf("args: path=README.md") == recordY + 1,
          "the expanded record's first detail line is its continuation, not a preview");
    check(expanded.drawnRows == records.size() + expanded.expandedLines.size(),
          "an expanded record costs its continuation lines, not a preview line");

    // Scroll accounting: every preview charges one body line, so a window that
    // cannot show all the lines scrolls by whole records.
    std::deque<model::RawLogRecord> detailed;
    for (int i = 0; i < 14; ++i)
        detailed.push_back(record(model::kRawLogLevelInfo, "detail row " + std::to_string(i), i,
                                  "args: n=" + std::to_string(i)));
    views::RawLogState scrolled = openSurface(rect, ringOf(detailed));
    // One more frame: the follower's pin lands at the bottom of a 28-line body in a
    // 27-row window, and the clamp is observable on the frame after it applies.
    frame([&](const ui::Metrics& m) { views::renderRawLog(scrolled, rect, m, ringOf(detailed)); });
    check(scrolled.firstVisible == 1,
          "28 body lines in a 27-row window scroll by one line, a preview's worth");

    // The preview row is its own hit target, and its click toggles the same record
    // the row toggles. One click at (x, y): the press and the release are
    // separate frames, which is how ImGui's ButtonBehavior activates an item.
    const auto clickAt = [&](int col, int rowY) {
        ImGui::GetIO().AddMousePosEvent(static_cast<float>(col), static_cast<float>(rowY));
        frame([&](const ui::Metrics& m) { views::renderRawLog(state, rect, m, ring); });
        ImGui::GetIO().AddMouseButtonEvent(0, true);
        frame([&](const ui::Metrics& m) { views::renderRawLog(state, rect, m, ring); });
        ImGui::GetIO().AddMouseButtonEvent(0, false);
        frame([&](const ui::Metrics& m) { views::renderRawLog(state, rect, m, ring); });
    };
    clickAt(msgCol + 2, previewY);
    check(state.expandedKey == views::rawLogKey(records[1]),
          "a click on the preview line opens the record");
    clickAt(msgCol + 2, recordY);
    check(state.expandedKey.empty(), "and a click on the record row closes it again");
    clickAt(msgCol + 2, previewY);
    check(state.expandedKey == views::rawLogKey(records[1]),
          "the toggle works in both directions, from the preview too");
    clickAt(msgCol + 2, previewY + 1);
    check(state.expandedKey == views::rawLogKey(records[1]),
          "a click on a continuation line toggles nothing");

    // At narrow widths the preview line is clipped at the body's edge like any
    // other drawn text: it is cut whole-cell-wise (the fixture's preview is ASCII)
    // and no glyph lands outside the body's cells.
    g_w = 40; // 36 body cells: rung 3, message column at body cell 20
    g_h = 30;
    const ui::Rect narrowRect = surfaceRect(g_w, g_h);
    views::RawLogState narrow = openSurface(narrowRect, ring);
    const Grid narrowGrid =
        frame([&](const ui::Metrics& m) { views::renderRawLog(narrow, narrowRect, m, ring); });
    const int narrowRecordY = narrowGrid.rowOf("tool args");
    check(narrowRecordY > 0, "premise: the narrow surface still shows the record");
    const std::string narrowPreview = narrowGrid.rowText(narrowRecordY + 1);
    check(narrowPreview.find("args:") != std::string::npos,
          "premise: the preview's first words still fit at the narrow width");
    check(narrowPreview.find("README.md") == std::string::npos,
          "the preview is cut at the body's edge, not painted past it");
    int narrowLast = static_cast<int>(narrowPreview.size()) - 1;
    while (narrowLast >= 0 && narrowPreview[narrowLast] == ' ')
        --narrowLast;
    check(narrowLast < 2 + (g_w - 4), "the preview's last cell sits inside the body");
    g_w = 100;
    g_h = 30;
}
/// The tag and the clock follow the plan's width decisions: only the widest rung
/// reserves tag room, a tag too wide for the message's reserved minimum is dropped
/// whole, and the clock is sliced to the rung's cells.
void checkTagAndShortClock() {
    // The widest rung: a tag whose own width would starve the message's reserved
    // minimum never draws, and the message keeps the full column.
    g_w = 100;
    g_h = 30;
    const ui::Rect rect = surfaceRect(g_w, g_h);
    model::RawLogRecord bigTag = record(model::kRawLogLevelError, std::string(40, 'x'), 0);
    bigTag.action = TuiLogAction_Retry;
    bigTag.actionPayload = "a payload long enough to overflow the tag slack";
    const std::deque<model::RawLogRecord> wide{bigTag};
    views::RawLogState state = openSurface(rect, ringOf(wide));
    const Grid grid = capture();
    check(!grid.hasText("action "), "a tag that would starve the message is dropped whole");
    // The message is longer than the body, so wide mode engages: the tag is dropped
    // whole and the message draws at its full natural width, the window itself doing
    // the clipping - the x's ride the body's right edge and no ellipsis appears. The
    // cells are read from the grid, not the row string: the marker is one cell but
    // three UTF-8 bytes, so a byte index would lie about where the column ends.
    const int clockY = grid.rowOf("00:00:00.000");
    int firstX = -1;
    int lastInk = -1;
    for (int x = 0; x < g_w - 2; ++x) {
        const std::string cell = utf8Of(grid.at(x, clockY));
        if (cell == "x" && firstX < 0)
            firstX = x;
        if (cell != " ")
            lastInk = x;
    }
    check(firstX > 0, "the wide row still draws its message");
    std::string column;
    for (int x = firstX; x <= lastInk; ++x)
        column += utf8Of(grid.at(x, clockY));
    check(!column.empty() && column.find_first_not_of('x') == std::string::npos,
          "the clipped message keeps its cells whole, no ellipsis with the tag");

    // A narrow body: the rung has no tag slack, so the tagged row shows no tag, both
    // messages still start at the same column, and the clock is sliced to eight cells.
    g_w = 54;
    g_h = 20;
    const ui::Rect narrow = surfaceRect(g_w, g_h);
    model::RawLogRecord unstamped = record(model::kRawLogLevelInfo, "session opened", 0);
    unstamped.timestampMs = 0;
    model::RawLogRecord tagged = record(model::kRawLogLevelError, "read_file failed", 1);
    tagged.action = TuiLogAction_Retry;
    tagged.actionPayload = "turn-7";
    const std::deque<model::RawLogRecord> pair{unstamped, tagged};
    views::RawLogState narrowState = openSurface(narrow, ringOf(pair));
    const Grid narrowGrid = capture();
    check(narrowGrid.hasText("--:--:--"), "premise: the unstamped clock is drawn");
    check(!narrowGrid.hasText("--:--:--.---"), "the clock is sliced to the rung's eight cells");
    check(!narrowGrid.hasText("action"), "a narrow body draws no tag");
    const int opened = static_cast<int>(
        narrowGrid.rowText(narrowGrid.rowOf("session opened")).find("session opened"));
    const int failed = static_cast<int>(
        narrowGrid.rowText(narrowGrid.rowOf("read_file failed")).find("read_file failed"));
    check(opened == failed, "the message column is shared at the narrow rung too");
}

/// The degradation ladder, behaviourally: one unstamped, actionless
/// record with a long message, driven through widths one cell apart at every rung
/// boundary. The rung reads the frame width alone, so segments appear and vanish
/// at exactly the widths the rung table names - and a segment's ink follows the
/// segment, never the width.
void checkLadder() {
    // One probe width per rung boundary: `+ 4` is the surface's padding, two
    // cells a side; 15 rows leave a 12-row body under title, filter and footer.
    const auto probe = [](int cells) {
        g_w = cells + 4;
        g_h = 15;
        model::RawLogRecord longRow = record(model::kRawLogLevelInfo, std::string(40, 'x'), 0);
        longRow.timestampMs = 0;
        const std::deque<model::RawLogRecord> one{longRow};
        views::RawLogState state = openSurface(surfaceRect(g_w, g_h), ringOf(one));
        (void)state;
        return capture();
    };

    // Rung 0 (84 cells and up): clock, location and tag slack all fit.
    check(probe(84).hasText("pool.d:412"), "84 cells: rung 0 draws the location");

    // Rung 1 (74-83): the tag slack goes, clock and location stay.
    const Grid noTag = probe(83);
    check(noTag.hasText("--:--:--.---") && noTag.hasText("pool.d:412"),
          "83 cells: rung 1 keeps the clock and the location");

    // Rung 2 (70-73): the clock slices to eight cells, the location stays.
    const Grid shortClock = probe(73);
    check(shortClock.hasText("--:--:--") && !shortClock.hasText("--:--:--.---") &&
              shortClock.hasText("pool.d:412"),
          "73 cells: the clock is sliced and the location stays");

    // Rung 3 (32-69): the location goes, the short clock stays.
    const Grid noLoc = probe(69);
    check(noLoc.hasText("--:--:--") && !noLoc.hasText("pool.d:412"),
          "69 cells: rung 3 keeps only the short clock");
    check(probe(32).hasText("--:--:--"), "32 cells: the short clock still draws");

    // Rung 4 (10-31): no clock, no location - level and message alone.
    const Grid bare = probe(31);
    check(!bare.hasText("--:--:--") && bare.hasText("info "),
          "31 cells: rung 4 keeps the level and the message");

    // The message's own degradation, one cell at a time at rung 4's floor: wide mode
    // has taken over the too-long message - it draws at its full natural width and the
    // window itself clips it, one visible cell fewer per narrower probe, and no
    // ellipsis ever appears. The cells the body hides ride behind the horizontal
    // scrollbar the wide plan engages.
    const std::string prefix = "   info   "; // marker cell, gap, level field, gap
    for (int cells = 10; cells <= 17; ++cells) {
        const Grid grid = probe(cells);
        const int y = grid.rowOf("info");
        check(y >= 0, "the marker and level still draw at " + std::to_string(cells) + " cells");
        const std::string row = grid.rowText(y);
        const int messageCells = cells - 10;
        const std::string expect = prefix + std::string(messageCells < 0 ? 0 : messageCells, 'x');
        check(row.compare(2, expect.size(), expect) == 0,
              "the message draws whole, window-clipped, at " + std::to_string(cells) + " cells");
    }

    // The lower rung edges too: a drifted table row would show through only at the
    // exact boundary width, so each edge is probed behaviourally as well.
    const Grid edgeRung1 = probe(74);
    check(edgeRung1.hasText("--:--:--.---") && edgeRung1.hasText("pool.d:412"),
          "74 cells: rung 1 keeps the full clock and the location");
    const Grid edgeRung2 = probe(70);
    check(edgeRung2.hasText("--:--:--") && !edgeRung2.hasText("--:--:--.---") &&
              edgeRung2.hasText("pool.d:412"),
          "70 cells: rung 2 keeps the short clock and the location");
    const Grid edgeRung3 = probe(33);
    check(edgeRung3.hasText("--:--:--") && !edgeRung3.hasText("pool.d:412"),
          "33 cells: rung 3 keeps the short clock without the location");

    // Below the plan's floor: the body draws no rows at all - not even a
    // fragment - while the surface itself stays up.
    const Grid floor = probe(9);
    check(!floor.hasText("info"), "9 cells: the body draws no record rows at all");
    check(floor.hasText("Raw Log"), "9 cells: the surface itself stays up");

    // The empty state stays usable at the floor: drawn, clipped like any row.
    g_w = 13; // nine cells of body
    g_h = 15;
    const std::deque<model::RawLogRecord> noRecords{};
    views::RawLogState emptyState = openSurface(surfaceRect(g_w, g_h), ringOf(noRecords));
    check(capture().hasText("No recor"), "the empty state still draws, clipped, at the floor");

    // Copy at the floor takes nothing: the body painted no rows, so neither the
    // button nor the `y` key may hand the clipboard a line the screen never showed.
    const std::deque<model::RawLogRecord> one{record(model::kRawLogLevelInfo, "a record", 0)};
    views::RawLogState floorCopy = openSurface(surfaceRect(g_w, g_h), ringOf(one));
    floorCopy.copiedNow = true;
    frame([&](const ui::Metrics& m) {
        views::renderRawLog(floorCopy, surfaceRect(g_w, g_h), m, ringOf(one));
    });
    check(floorCopy.drawnRows == 0, "premise: the floor drew no rows");
    check(floorCopy.copiedRows.empty(), "a copy at the floor puts nothing on the clipboard");

    // A body with no cells at all, and a surface with no width: both render
    // without crashing and draw no record row.
    for (const int w : {0, 4, 13}) {
        g_w = w;
        g_h = 15;
        const std::deque<model::RawLogRecord> one{record(model::kRawLogLevelInfo, "probe", 0)};
        openSurface(surfaceRect(g_w, g_h), ringOf(one));
        check(!capture().hasText("probe"),
              "a body with no cells draws no record rows (w=" + std::to_string(w) + ")");
    }

    // Same width, different content: the rung reads the frame alone, so two
    // records whose metadata differ in length draw their message at the same
    // column, and the tag that does not fit changes no other row's geometry.
    g_w = 88; // 84 cells
    g_h = 15;
    model::RawLogRecord plain = record(model::kRawLogLevelInfo, "first row", 0);
    plain.timestampMs = 0;
    model::RawLogRecord deep = record(model::kRawLogLevelInfo, "second row", 1);
    deep.timestampMs = 0;
    deep.module = "llm.agent.pool.deeper.module.path";
    deep.function = "processLongerTurn";
    deep.file = "source/llm/agent/pool/deeper/path.d";
    deep.line = 9001;
    deep.action = TuiLogAction_Retry;
    deep.actionPayload = "turn-7";
    const std::deque<model::RawLogRecord> pair{plain, deep};
    views::RawLogState pairState = openSurface(surfaceRect(g_w, g_h), ringOf(pair));
    const Grid pairGrid = capture();
    const int firstX =
        static_cast<int>(pairGrid.rowText(pairGrid.rowOf("first row")).find("first row"));
    const int secondX =
        static_cast<int>(pairGrid.rowText(pairGrid.rowOf("second row")).find("second row"));
    check(firstX >= 0 && firstX == secondX,
          "the message column is content-independent across rows");

    // The inks follow the segments, not the widths: the sliced clock still
    // carries textLow at its first drawn cell, and the message still carries its
    // level ink at its first cell.
    g_w = 73; // rung 3: sixty-nine cells of body
    g_h = 15;
    model::RawLogRecord inkRow = record(model::kRawLogLevelInfo, std::string(40, 'x'), 0);
    inkRow.timestampMs = 0;
    const std::deque<model::RawLogRecord> oneInk{inkRow};
    const model::RawLogRing inkRing = ringOf(oneInk);
    views::RawLogState inkState = openSurface(surfaceRect(g_w, g_h), inkRing);
    const Grid inkBlank = capture();
    const int inkY = inkBlank.rowOf("info");
    check(inkY >= 0 && inkY + 2 < g_h &&
              (inkBlank.at(4, inkY + 2) == 0 || inkBlank.at(4, inkY + 2) == ' '),
          "premise: the ink probe row is blank");
    const Grid inkGrid = frame([&](const ui::Metrics& mm) {
        views::renderRawLog(inkState, surfaceRect(g_w, g_h), mm, inkRing);
        probeToken(mm, 4, inkY + 2, ui::ColorToken::textLow);
        probeToken(mm, 7, inkY + 2, ui::ColorToken::textHi);
    });
    check(inkGrid.fgAt(5, inkY) == inkGrid.fgAt(4, inkY + 2),
          "the sliced clock still carries the low ink at its first cell");
    check(inkGrid.fgAt(22, inkY) == inkGrid.fgAt(7, inkY + 2),
          "and the message still carries its level ink at rung 4's floor");
}

void checkSave() {
    const std::string root = scratchRoot();
    check(!root.empty(), "the scratch data directory was created");
    setenv("XDG_DATA_HOME", root.c_str(), 1);

    const std::deque<model::RawLogRecord> records = mixedRecords();
    const model::RawLogRing ring = ringOf(records);
    check(model::rawLogSaveDir() == root + "/llmfun/logs",
          "the save directory is the data directory, never the workarea");

    const std::int64_t stamp = kFixtureEpochMs;
    check(model::rawLogSaveName(stamp) == "ui-20260621-000000.log",
          "the file name is the design's ui-YYYYMMDD-HHMMSS.log");
    check(model::rawLogSaveName(stamp, 2) == "ui-20260621-000000-2.log",
          "and a collision gets a suffix, never an overwrite");

    std::string path;
    std::string error;
    check(model::rawLogSave(ring, "", stamp, path, error), "the save writes a file");
    check(path == root + "/llmfun/logs/ui-20260621-000000.log", "at the expected path");
    const std::string content = readWhole(path);
    check(contains(content, "session opened"), "the file carries the records");
    check(contains(content, "args: path=README.md"), "and their detail");
    check(contains(content, "error"), "and the severity words");

    std::string second;
    check(model::rawLogSave(ring, "", stamp, second, error),
          "a second save in the same second still writes");
    check(second != path && contains(second, "-1.log"), "to a new file, never over the first");

    std::string filteredPath;
    check(model::rawLogSave(ring, "read_file", stamp + 1000, filteredPath, error),
          "a filtered save writes");
    const std::string filtered = readWhole(filteredPath);
    check(contains(filtered, "read_file failed"), "the matching record");
    check(!contains(filtered, "session opened"), "and only the matching one");

    // Restore the environment for anything after this.
    unsetenv("XDG_DATA_HOME");
}

// ---------------------------------------------------------------------------
// 3. The surface
// ---------------------------------------------------------------------------

void checkSurface() {
    const std::deque<model::RawLogRecord> records = mixedRecords();
    const model::RawLogRing ring = ringOf(records);
    g_w = 100;
    g_h = 30;
    const ui::Rect rect = surfaceRect(g_w, g_h);
    views::RawLogState state = openSurface(rect, ring);
    check(state.matchingRecords == records.size(), "every record passes the trace floor");

    g_w = 100;
    g_h = 30;
    const Grid grid =
        frame([&](const ui::Metrics& m) { views::renderRawLog(state, rect, m, ring); });
    check(grid.hasText(views::kRawLogTitle), "the title row says Raw Log");
    check(grid.hasText(views::kRawLogChipFollow), "the follower's chip is up while following");
    check(grid.hasText("session opened"), "an info record is drawn");
    check(grid.hasText("read_file failed"), "an error record is drawn");
    check(grid.hasText("agent.pool.processTurn"), "the developer coordinates are drawn");
    check(grid.hasText("pool.d:412"), "including the source line");
    check(grid.hasText(views::kRawLogFilterHint), "the filter field is up");
    check(grid.hasText(views::kRawLogCopyLabel) && grid.hasText(views::kRawLogSaveLabel) &&
              grid.hasText(views::kRawLogCloseLabel),
          "and the three controls");
    check(grid.hasText(views::kRawLogHints), "the overlay lists its keys");
    // The closed record with detail draws its preview line too, so the
    // drawn bound is records plus their preview lines, not just the records.
    const std::size_t previewLines = 1; // mixedRecords: one closed record with detail
    check(state.drawnRows == records.size() + previewLines,
          "the body drew the records it has, plus their preview lines");
}

void checkFilterAndStates() {
    const std::deque<model::RawLogRecord> records = mixedRecords();
    const model::RawLogRing ring = ringOf(records);
    g_w = 100;
    g_h = 30;
    const ui::Rect rect = surfaceRect(g_w, g_h);

    views::RawLogState filtered = openSurface(rect, ring);
    std::snprintf(filtered.filter.data(), filtered.filter.size(), "%s", "read_file");
    ++filtered.filterSeq;
    const Grid narrowed =
        frame([&](const ui::Metrics& m) { views::renderRawLog(filtered, rect, m, ring); });
    check(filtered.matchingRecords == 1, "the filter narrows the list to the matching record");
    check(narrowed.hasText("read_file failed"), "which is drawn");
    check(!narrowed.hasText("session opened"), "and the others are not");

    views::RawLogState noMatch = openSurface(rect, ring);
    std::snprintf(noMatch.filter.data(), noMatch.filter.size(), "%s", "zzz-no-such-thing");
    ++noMatch.filterSeq;
    const Grid empty =
        frame([&](const ui::Metrics& m) { views::renderRawLog(noMatch, rect, m, ring); });
    check(noMatch.matchingRecords == 0, "a filter that hides everything matches nothing");
    check(empty.hasText(views::kRawLogNoMatches), "and the body says `No records match`");
    check(!empty.hasText(views::kRawLogEmpty), "not the closed ring's empty state");

    const std::deque<model::RawLogRecord> none;
    const model::RawLogRing emptyRing = ringOf(none);
    views::RawLogState closed = openSurface(rect, emptyRing);
    const Grid blank =
        frame([&](const ui::Metrics& m) { views::renderRawLog(closed, rect, m, emptyRing); });
    check(blank.hasText(views::kRawLogEmpty), "a ring with no records says `No records yet`");

    // A paused surface says so, and the dropped counter is a row of its own (NFR-17).
    views::RawLogState paused = openSurface(rect, ringOf(records, 3));
    paused.paused = true;
    const Grid pausedGrid = frame(
        [&](const ui::Metrics& m) { views::renderRawLog(paused, rect, m, ringOf(records, 3)); });
    check(pausedGrid.hasText(views::kRawLogChipPaused), "a paused surface shows the `paused` chip");
    check(pausedGrid.hasText(std::string("3") + views::kRawLogDroppedSuffix),
          "and the dropped counter is drawn when it advances");
}

/// The filter highlight paints the first case-insensitive occurrence of the
/// filter inside the message with the accent ink while the rest of the message
/// keeps the level's severity ink. The other segments are never highlighted,
/// an empty filter highlights nothing, and a match that survives only in the
/// clipped-off tail stays plain.
void checkFilterHighlight() {
    g_w = 100;
    g_h = 30;
    const ui::Rect rect = surfaceRect(g_w, g_h);
    const std::deque<model::RawLogRecord> records = mixedRecords();
    const model::RawLogRing ring = ringOf(records);

    const auto withFilter = [&](const char* filter, const model::RawLogRing& r) {
        views::RawLogState state = openSurface(rect, r);
        std::snprintf(state.filter.data(), state.filter.size(), "%s", filter);
        ++state.filterSeq;
        return state;
    };
    const auto blankFrame = [&](views::RawLogState& state, const model::RawLogRing& r) {
        return frame([&](const ui::Metrics& m) { views::renderRawLog(state, rect, m, r); });
    };
    const auto colIn = [](const Grid& g, int y, const char* needle) {
        const std::size_t at = g.rowText(y).find(needle);
        return at == std::string::npos ? -1 : static_cast<int>(at);
    };

    // One record survives the filter, so the row under the warn record is blank
    // in the very frame the highlight draws.
    views::RawLogState mid = withFilter("read-only", ring);
    const Grid midBlank = blankFrame(mid, ring);
    check(mid.matchingRecords == 1, "premise: the filter narrows the list to the warn record");
    const int warnY = midBlank.rowOf("ui.conf is read-only");
    check(warnY > 0, "premise: the filtered warn row is on screen");
    check(midBlank.rowText(warnY).find("ui.conf is read-only") != std::string::npos,
          "the highlight leaves the message's characters alone");
    const int probeY = warnY + 2;
    check(midBlank.at(6, probeY) == ' ', "premise: the probe row is blank");
    check(midBlank.at(8, probeY) == ' ', "premise: the probe row is blank");
    check(midBlank.at(10, probeY) == ' ', "premise: the probe row is blank");

    const Grid midGrid = frame([&](const ui::Metrics& m) {
        views::renderRawLog(mid, rect, m, ring);
        probeToken(m, 6, probeY, ui::ColorToken::textLow);
        probeToken(m, 8, probeY, ui::ColorToken::stateWarn);
        probeToken(m, 10, probeY, ui::ColorToken::accent);
    });
    const int msgCol = colIn(midGrid, warnY, "ui.conf is read-only");
    check(msgCol > 0, "premise: the message column is found");
    check(midGrid.rowText(warnY).compare(msgCol + 11, 9, "read-only") == 0,
          "premise: the match sits where the filter matched");
    check(midGrid.fgAt(msgCol + 11, warnY) == midGrid.fgAt(10, probeY),
          "the matched substring carries the accent ink");
    check(midGrid.fgAt(msgCol + 19, warnY) == midGrid.fgAt(10, probeY),
          "and so does its last cell");
    check(midGrid.fgAt(msgCol, warnY) == midGrid.fgAt(8, probeY),
          "the message before the match keeps the level's ink");
    check(midGrid.fgAt(msgCol + 20, warnY) != midGrid.fgAt(10, probeY),
          "and nothing past the match carries the accent");
    const int clockCol = colIn(midGrid, warnY, "00:00:02.000");
    check(clockCol > 0, "premise: the clock is drawn");
    check(midGrid.fgAt(clockCol, warnY) == midGrid.fgAt(6, probeY), "the clock is not highlighted");
    const int locCol = colIn(midGrid, warnY, "agent.pool.processTurn");
    check(locCol > 0, "premise: the location is drawn");
    check(midGrid.fgAt(locCol, warnY) == midGrid.fgAt(6, probeY), "nor the location");

    // The filter matches case-insensitively, so an upper-case filter finds the
    // same record and highlights the same span.
    views::RawLogState upper = withFilter("READ-ONLY", ring);
    const Grid upperGrid = frame([&](const ui::Metrics& m) {
        views::renderRawLog(upper, rect, m, ring);
        probeToken(m, 10, probeY, ui::ColorToken::accent);
    });
    check(upper.matchingRecords == 1, "the upper-case filter matches the same record");
    check(upperGrid.fgAt(msgCol + 11, warnY) == upperGrid.fgAt(10, probeY),
          "and highlights the same span");

    // A match at the message's very start: the span begins in the first
    // message cell and the cell after it is plain again.
    views::RawLogState head = withFilter("ui.conf", ring);
    const Grid headGrid = frame([&](const ui::Metrics& m) {
        views::renderRawLog(head, rect, m, ring);
        probeToken(m, 8, probeY, ui::ColorToken::stateWarn);
        probeToken(m, 10, probeY, ui::ColorToken::accent);
    });
    check(head.matchingRecords == 1, "premise: the head filter keeps the warn record");
    check(headGrid.fgAt(msgCol, warnY) == headGrid.fgAt(10, probeY),
          "a match at the message start highlights from the first cell");
    check(headGrid.fgAt(msgCol + 7, warnY) != headGrid.fgAt(10, probeY),
          "and the cell after the span is plain again");

    // An empty filter draws the message as one plain severity-colored run. The
    // unfiltered layout carries more rows, so the probes live in the warn
    // row's own message-box tail, past the twenty-character message.
    views::RawLogState plain = openSurface(rect, ring);
    const Grid plainBlank = blankFrame(plain, ring);
    const int plainWarn = plainBlank.rowOf("ui.conf is read-only");
    check(plainWarn > 0, "premise: the unfiltered warn row is on screen");
    const int plainMsg = colIn(plainBlank, plainWarn, "ui.conf is read-only");
    check(plainMsg > 0, "premise: the unfiltered message column is found");
    check(plainBlank.at(plainMsg + 30, plainWarn) == ' ', "premise: the message tail is blank");
    check(plainBlank.at(plainMsg + 31, plainWarn) == ' ', "premise: the message tail is blank");
    const Grid plainGrid = frame([&](const ui::Metrics& m) {
        views::renderRawLog(plain, rect, m, ring);
        probeToken(m, plainMsg + 30, plainWarn, ui::ColorToken::stateWarn);
        probeToken(m, plainMsg + 31, plainWarn, ui::ColorToken::accent);
    });
    check(plainGrid.fgAt(plainMsg, plainWarn) == plainGrid.fgAt(plainMsg + 30, plainWarn),
          "an empty filter paints the whole message in the level's ink");
    check(plainGrid.fgAt(plainMsg, plainWarn) != plainGrid.fgAt(plainMsg + 31, plainWarn),
          "and nothing carries the accent ink");

    // A blanks-only filter is match-all for the matcher, not a filter: every row
    // is shown, and the highlight agrees by painting nothing.
    views::RawLogState spaces = withFilter(" \t", ring);
    const Grid spacesBlank = blankFrame(spaces, ring);
    check(spaces.matchingRecords == records.size(),
          "the blanks-only filter is match-all, like the matcher says");
    check(spacesBlank.rowOf("ui.conf is read-only") == plainWarn,
          "premise: the blanks-only layout is the unfiltered one");
    const Grid spacesGrid = frame([&](const ui::Metrics& m) {
        views::renderRawLog(spaces, rect, m, ring);
        probeToken(m, plainMsg + 30, plainWarn, ui::ColorToken::stateWarn);
        probeToken(m, plainMsg + 31, plainWarn, ui::ColorToken::accent);
    });
    bool painted = false;
    for (int c = plainMsg; c < plainMsg + 38 && !painted; ++c) {
        if (c == plainMsg + 31)
            continue; // the probe cell itself
        painted = spacesGrid.fgAt(c, plainWarn) == spacesGrid.fgAt(plainMsg + 31, plainWarn);
    }
    check(!painted, "a blanks-only filter highlights nothing");
    check(spacesGrid.fgAt(plainMsg, plainWarn) == spacesGrid.fgAt(plainMsg + 30, plainWarn),
          "and the message keeps the level's ink");

    // A match that survives only in the clipped-off tail stays plain: the
    // record still passes the filter because the match hides in the full
    // message, but nothing on screen carries the accent.
    std::deque<model::RawLogRecord> tailRecords;
    tailRecords.push_back(record(model::kRawLogLevelInfo, std::string(60, 'x') + "needle", 0));
    const model::RawLogRing tailRing = ringOf(tailRecords);
    views::RawLogState tail = withFilter("needle", tailRing);
    check(tail.matchingRecords == 1, "the filter still finds the record behind the cut");
    const Grid tailBlank = blankFrame(tail, tailRing);
    const int tailY = tailBlank.rowOf("00:00:00.000");
    check(tailY > 0, "premise: the filtered record row is on screen");
    check(tailBlank.rowText(tailY).find("needle") == std::string::npos,
          "premise: the match itself is clipped off the row");
    const int tailMsg = colIn(tailBlank, tailY, "xxx");
    check(tailMsg > 0, "premise: the clipped message column is found");
    const int tailProbeY = tailY + 2;
    check(tailBlank.at(8, tailProbeY) == ' ', "premise: the probe row is blank");
    check(tailBlank.at(10, tailProbeY) == ' ', "premise: the probe row is blank");
    const Grid tailGrid = frame([&](const ui::Metrics& m) {
        views::renderRawLog(tail, rect, m, tailRing);
        probeToken(m, 8, tailProbeY, ui::ColorToken::textHi);
        probeToken(m, 10, tailProbeY, ui::ColorToken::accent);
    });
    bool strayed = false;
    for (int c = tailMsg; c < tailMsg + 38 && !strayed; ++c)
        strayed = tailGrid.fgAt(c, tailY) == tailGrid.fgAt(10, tailProbeY);
    check(!strayed, "a match hidden in the clipped tail highlights nothing");
    check(tailGrid.fgAt(tailMsg, tailY) == tailGrid.fgAt(8, tailProbeY),
          "and the shown message keeps the level's ink");
    const std::string tailRow = tailGrid.rowText(tailY);
    int last = static_cast<int>(tailRow.size()) - 1;
    while (last >= 0 && tailRow[last] == ' ')
        --last;
    check(tailGrid.fgAt(last, tailY) == tailGrid.fgAt(8, tailProbeY), "through to the ellipsis");
}

void checkBoundsAndExpansionAndCopy() {
    // 5000 records - the ring's capacity - must draw a viewport, not the list (NFR-18).
    std::deque<model::RawLogRecord> many;
    for (int i = 0; i < 5000; ++i)
        many.push_back(record(model::kRawLogLevelInfo, "record " + std::to_string(i), i));
    const model::RawLogRing ring = ringOf(many);
    g_w = 100;
    g_h = 30;
    const ui::Rect rect = surfaceRect(g_w, g_h);
    views::RawLogState state = openSurface(rect, ring);
    g_w = 100;
    g_h = 30;
    frame([&](const ui::Metrics& m) { views::renderRawLog(state, rect, m, ring); });
    check(state.drawnRows <= 30, "5000 records draw at most a display's worth of rows (" +
                                     std::to_string(state.drawnRows) + ")");
    check(state.matchingRecords == 5000, "while the index knows all of them");

    // The follower opened on the newest record: the last record's line is on screen.
    const Grid tail = capture();
    check(tail.hasText("record 4999"), "the follower's catch-up shows the newest record");

    // The expanded record: its detail lines are rows under it, and the copy takes them too.
    views::RawLogState expanded = openSurface(rect, ringOf(mixedRecords()));
    expanded.expandedKey = views::rawLogKey(mixedRecords()[1]);
    g_w = 100;
    g_h = 30;
    const Grid detailGrid = frame([&](const ui::Metrics& m) {
        views::renderRawLog(expanded, rect, m, ringOf(mixedRecords()));
    });
    check(detailGrid.hasText("path=README.md"), "an expanded record draws its detail lines");
    check(expanded.expandedAt != std::string::npos, "and the index knows where it sits");

    views::RawLogState copied = openSurface(rect, ringOf(mixedRecords()));
    copied.firstVisible = 0;
    copied.windowRows = 3;
    const std::string payload = views::rawLogVisibleText(copied, ringOf(mixedRecords()), 0, 3);
    check(contains(payload, "session opened"), "the copy payload is the visible window");
    // windowRows=3 covers the first record's row, its preview line and the
    // second record's row. The preview takes a screen row but copies nothing - Copy
    // takes the canonical one-line form, never a detail preview - so only two of the
    // three rows reach the clipboard.
    check(!contains(payload, "ui.conf is read-only"), "a preview line copies nothing");
    check(!contains(payload, "read_file failed"), "and not the rows below the fold");

    // The copy bound and the drawn bound are *different* numbers (review I1): the body paints one
    // overscan row that a user cannot see, and `y` must not put it on the clipboard. After a real
    // frame, the payload is exactly `windowRows` lines and never more than what was drawn.
    views::RawLogState live = openSurface(rect, ring);
    live.copiedNow = true;
    frame([&](const ui::Metrics& m) { views::renderRawLog(live, rect, m, ring); });
    std::size_t copyLines = 0;
    for (const char c : live.copiedRows) {
        if (c == '\n')
            ++copyLines;
    }
    check(copyLines == live.windowRows, "y copies exactly the viewport rows (" +
                                            std::to_string(copyLines) + " of " +
                                            std::to_string(live.windowRows) + ")");
    // And the set is exact: these records carry no detail, so every drawn line is a
    // record row, and the payload is the canonical one-line form of each visible
    // record, concatenated in screen order.
    std::string expected;
    for (std::size_t i = live.firstVisible;
         i < ring.size() && i < live.firstVisible + live.windowRows; ++i) {
        expected += model::rawLogRowText(*ring.at(i));
        expected += '\n';
    }
    check(live.copiedRows == expected, "the payload is exactly the visible record rows");
    check(copyLines <= live.drawnRows, "and never copies a row it did not draw (" +
                                           std::to_string(copyLines) + " copied, " +
                                           std::to_string(live.drawnRows) + " drawn)");
    check(contains(live.copiedRows, "record 4999"),
          "the copy reaches the newest record at the bottom");
    check(contains(live.copiedRows, "record 4973"),
          "its first line is the first painted row, not the one above it");
    check(!contains(live.copiedRows, "record 4972"),
          "and the row above the painted window is not copied");

    // A ring where a record *does* carry detail: its preview line occupies a screen
    // row (the scroll account charges it) but contributes nothing to the payload.
    const std::deque<model::RawLogRecord> mixed = mixedRecords();
    views::RawLogState mixedLive = openSurface(rect, ringOf(mixed));
    mixedLive.copiedNow = true;
    frame([&](const ui::Metrics& m) { views::renderRawLog(mixedLive, rect, m, ringOf(mixed)); });
    expected.clear();
    for (std::size_t i = 0; i < mixed.size(); ++i) {
        expected += model::rawLogRowText(mixed[i]);
        expected += '\n';
    }
    check(mixedLive.copiedRows == expected,
          "preview lines take a screen row and copy nothing (4 records, 5 drawn rows)");
    check(mixedLive.drawnRows == mixed.size() + 1, "the preview row was drawn");
}

/// The ring rotates under a *stable* filter (review I2). At capacity the deque keeps its size, and
/// a burst stamps records inside one millisecond - so neither the size nor the front timestamp can
/// tell the surface that a record arrived. The accept counter can, and the filtered index must not
/// keep pointing one record later.
void checkRingRotation() {
    std::deque<model::RawLogRecord> ringData;
    for (int i = 0; i < 10; ++i) {
        model::RawLogRecord r = record(model::kRawLogLevelInfo, "record " + std::to_string(i), 0);
        // Paired stamps: 0,0,1,1,2,2,...
        r.timestampMs = kFixtureEpochMs + 1000LL * (i / 2);
        ringData.push_back(r);
    }
    std::size_t arrivals = 10;
    const auto ring = [&]() { return model::RawLogRing{&ringData, 0, arrivals}; };
    g_w = 100;
    g_h = 30;
    const ui::Rect rect = surfaceRect(g_w, g_h);
    views::RawLogState state = openSurface(rect, ring());
    std::snprintf(state.filter.data(), state.filter.size(), "%s", "record 3");
    ++state.filterSeq;
    const Grid before =
        frame([&](const ui::Metrics& m) { views::renderRawLog(state, rect, m, ring()); });
    check(before.hasText("record 3"), "the filter finds its record");
    check(!before.hasText("record 4"), "and hides its neighbour");

    // A rotation inside the same millisecond: the evicted front and the new front share a stamp.
    const std::int64_t frontStamp = ringData.front().timestampMs;
    model::RawLogRecord fresh = record(model::kRawLogLevelInfo, "record 10", 0);
    fresh.timestampMs = frontStamp;
    ringData.push_back(fresh);
    ringData.pop_front();
    ++arrivals;
    const Grid after =
        frame([&](const ui::Metrics& m) { views::renderRawLog(state, rect, m, ring()); });
    check(after.hasText("record 3"), "the filtered record is still the one the user asked for");
    check(!after.hasText("record 4"), "a rotated ring does not slide the index onto the neighbour");
}

/// The `UiLog` floor, the absolute-path rule and the action pair, at the model level (reviews I4,
/// M5).
void checkModelRules() {
    model::RawLogRecord retryable = record(model::kRawLogLevelError, "read_file failed", 0);
    retryable.action = 1; // TuiLogAction_Retry
    retryable.actionPayload = "turn-7";
    const std::string row = model::rawLogRowText(retryable);
    check(contains(row, "action 1 turn-7"),
          "the raw row carries the action id and its payload (design: Raw Log = all fields)");
    check(model::rawLogMatches(retryable, "turn-7"),
          "so the filter can find the turn a retryable error belongs to");
    model::RawLogRecord plain = record(model::kRawLogLevelError, "read_file failed", 0);
    check(!contains(model::rawLogRowText(plain), "action"),
          "and a record with no action prints no empty action segment");

    // A relative data directory would put the log under the CWD - the workarea rule forbids it.
    setenv("XDG_DATA_HOME", "relative/data", 1);
    std::string path;
    std::string error;
    const std::deque<model::RawLogRecord> records = mixedRecords();
    check(!model::rawLogSave(ringOf(records), "", kFixtureEpochMs, path, error),
          "a relative data directory is refused");
    check(path.empty() && contains(error, "absolute"), "and the answer names the reason");
    unsetenv("XDG_DATA_HOME");
}

/// The drawer's grip and the height it hands back (design §4.9 "resizable", review I3).
void checkDrawerResize() {
    const ui::Metrics gui = ui::metricsFor(false, 1.0f, 1.0f);
    const float minH = 4.0f * gui.row;
    ui::LayoutPrefs prefs;
    prefs.rawLogOpen = true;
    const ui::ShellLayout byDefault = ui::computeShell(gui, ImVec2(1280.0f, 820.0f), prefs);
    check(byDefault.rawLogOverlay.h >= minH, "the drawer starts at the design's 30 %");

    prefs.rawLogH = 2.0f * gui.row; // a drag to nothing
    const ui::ShellLayout floored = ui::computeShell(gui, ImVec2(1280.0f, 820.0f), prefs);
    check(floored.rawLogOverlay.h >= minH, "a drag below the minimum is floored at four rows");

    prefs.rawLogH = 4000.0f; // a drag past the display
    const ui::ShellLayout capped = ui::computeShell(gui, ImVec2(1280.0f, 820.0f), prefs);
    check(capped.rawLogOverlay.h <= capped.rawLogBand.h + 0.5f,
          "a drag past the band is clamped by the layout, not by the view");
    check(capped.rawLogOverlay.bottom() <= capped.composer.y + 0.5f,
          "so no drag can cover the composer");
    check(capped.thread.h >= 3.0f * gui.row - 0.5f, "and none can squeeze the thread's three rows");

    prefs.rawLogH = 12.0f * gui.row;
    const ui::ShellLayout grown = ui::computeShell(gui, ImVec2(1280.0f, 820.0f), prefs);
    check(grown.rawLogOverlay.h > byDefault.rawLogOverlay.h + 0.5f,
          "a drag upward really grows the drawer");
}

void checkFollower() {
    // A quiet stream pins nothing; the pin is content-driven, not a per-frame `SetScrollHereY`.
    std::deque<model::RawLogRecord> few;
    for (int i = 0; i < 3; ++i)
        few.push_back(record(model::kRawLogLevelInfo, "row " + std::to_string(i), i));
    g_w = 100;
    g_h = 30;
    const ui::Rect rect = surfaceRect(g_w, g_h);
    views::RawLogState quiet;
    quiet.open = true;
    quiet.overlayShape = true;
    quiet.minLevel = model::kRawLogLevelTrace;
    for (int i = 0; i < 6; ++i)
        frame([&](const ui::Metrics& m) { views::renderRawLog(quiet, rect, m, ringOf(few)); });
    check(quiet.followPins == 0, "three records on a 30-row surface never move the follower");

    // A busy stream: 5000 records, so it opens scrolled to the end - and the pin is capped.
    std::deque<model::RawLogRecord> many;
    for (int i = 0; i < 5000; ++i)
        many.push_back(record(model::kRawLogLevelInfo, "record " + std::to_string(i), i));
    views::RawLogState busy;
    busy.open = true;
    busy.overlayShape = true;
    busy.minLevel = model::kRawLogLevelTrace;
    for (int i = 0; i < 6; ++i)
        frame([&](const ui::Metrics& m) { views::renderRawLog(busy, rect, m, ringOf(many)); });
    check(busy.followPins >= 1, "a stream longer than the fold is pinned to its newest record");
    // Six frames at 60 fps are 100 ms of clock, so at most two pins may have run (the cap is one
    // per 100 ms); the point of the check is that it is not six.
    check(busy.followPins <= 2, "and the 10 fps cap keeps the pin off every frame (" +
                                    std::to_string(busy.followPins) + " pins in 6 frames)");

    // Paused: the pin does not run at all.
    views::RawLogState paused = busy;
    paused.paused = true;
    paused.followPins = 0;
    paused.nextPinAt = 0.0;
    for (int i = 0; i < 6; ++i)
        frame([&](const ui::Metrics& m) { views::renderRawLog(paused, rect, m, ringOf(many)); });
    check(paused.followPins == 0, "a paused surface never moves");

    // `f` re-arms: the one-shot latch, and the follow request the frame applies.
    views::RawLogState asked;
    views::rawLogFollowNewest(asked);
    check(asked.followRequested, "the follow action raises the frame's request");
    check(asked.nextPinAt == 0.0, "and owes an immediate catch-up");
}

// ---------------------------------------------------------------------------
// 4. The frame wiring
// ---------------------------------------------------------------------------

tui::TuiState workbenchState() {
    tui::TuiState state;
    return state;
}

Grid shellFrame(tui::TuiState& state, int w, int h) {
    g_w = w;
    g_h = h;
    return frame([&](const ui::Metrics&) {
        views::renderShell(state, ImVec2(static_cast<float>(w), static_cast<float>(h)));
    });
}

/// A frame with both layers: the shell, then the overlay stack (the palette and the Raw Log
/// overlay). `shellFrame()` alone cannot test a stack.
Grid overlayFrame(tui::TuiState& state, int w, int h, const char* chars = nullptr) {
    g_w = w;
    g_h = h;
    return frame(
        [&](const ui::Metrics&) {
            views::ShellFrame shell =
                views::renderShell(state, ImVec2(static_cast<float>(w), static_cast<float>(h)));
            views::renderOverlays(state, shell);
        },
        chars);
}

void checkAccess() {
    tui::TuiState state = workbenchState();
    shellFrame(state, 100, 30);
    check(!state.ui.rawLogOpen, "the Raw Log is closed on every start (D-RAWLOG)");

    // A typed `/rawlog` is a command: it opens the surface, is never submitted and never joins
    // the history (the typed `/stop` rule). The frame's submit path calls the same function.
    state.userQuery.inputBuf = "/rawlog";
    state.composer.submit = true;
    check(views::handleRawLogCommand(state, state.userQuery.inputBuf),
          "a typed /rawlog is recognised as the command");
    check(state.ui.rawLogOpen, "and opens the surface");
    check(!state.userQuery.submitReady, "and is never submitted as a query");
    check(state.userQuery.inputBuf.empty(), "the command clears the draft like a send");
    check(state.userQuery.inputHistory.empty(), "and does not enter the history");
    check(!views::handleRawLogCommand(state, "/rawlog please"),
          "a query that merely starts with the word is a query");
    check(state.ui.rawLogOpen, "and leaves the surface alone");
    check(views::handleRawLogCommand(state, "/rawlog  "),
          "a trailing blank after the command is tolerated");
    check(!state.ui.rawLogOpen, "and a second /rawlog closes the surface");

    // `Ctrl+Shift+L`'s action, the palette's developer row and `--debug-ui` all land here.
    views::toggleRawLog(state);
    check(state.ui.rawLogOpen, "the toggle opens it");
    views::toggleRawLog(state);
    check(!state.ui.rawLogOpen, "and closes it");

    // The overlay step and the top of the stack: on the grid the surface is a modal overlay. The
    // shape is the *frame's* derivation (`deriveRawLog()` reads the metrics), so a frame runs
    // between the toggle and the questions about it - which is also the real sequence: a press
    // opens the surface on the next frame.
    views::toggleRawLog(state);
    shellFrame(state, 100, 30);
    check(state.rawLog.overlayShape, "the text grid's shape is the full-screen overlay");
    check(state.overlayOwnsKeyboard(), "which owns the keyboard while it is up");
    check(views::escStepClosesOverlay(state), "so Esc closes it");
    check(views::closeTopOverlay(state) == ui::OverlayLayer::RawLog, "and it is the top overlay");
    check(!state.ui.rawLogOpen, "after which it is closed");
    check(!state.overlayOwnsKeyboard(), "and the keyboard is the shell's again");

    // A closed surface starts clean: the filter is cleared (its widget id moves on) and the
    // drawer's dragged height goes back to the design's 30 % (review M1).
    state.ui.rawLogOpen = true;
    std::snprintf(state.rawLog.filter.data(), state.rawLog.filter.size(), "%s", "stale");
    state.ui.rawLogH = 400.0f;
    const int seqBefore = state.rawLog.filterSeq;
    views::closeRawLog(state);
    check(state.rawLog.filter[0] == '\0', "closing clears the filter");
    check(state.rawLog.filterSeq == seqBefore + 1, "and moves the field's id on");
    check(state.ui.rawLogH == 0.0f, "and forgets the dragged height");
}

void checkOverlayAndDrawer() {
    // The grid's overlay covers the display, including the now bar: that is what "full-screen"
    // means, and it is why the now bar's badge test draws the row directly.
    tui::TuiState state = workbenchState();
    state.ui.rawLogOpen = true;
    state.logRecords.push_back(record(model::kRawLogLevelInfo, "overlay covers the bar", 0));
    const Grid overlay = shellFrame(state, 100, 30);
    const int barRow = state.rawLog.open ? 29 : -1;
    check(overlay.hasText(views::kRawLogTitle), "the overlay draws its title over the shell");
    check(overlay.hasText("overlay covers the bar"), "and its records");
    check(barRow == 29 && !contains(overlay.rowText(barRow), "Ctrl+K commands"),
          "the now bar's hints are not visible under it");

    // The pixel profile's drawer is part of the tiling: it must never cover the composer.
    const ui::Metrics gui = ui::metricsFor(false, 1.0f, 1.0f);
    ui::LayoutPrefs prefs;
    prefs.rawLogOpen = true;
    const ui::ShellLayout layout = ui::computeShell(gui, ImVec2(1280.0f, 820.0f), prefs);
    check(layout.rawLogOverlay.w > 0.0f && layout.rawLogOverlay.h > 0.0f,
          "the GUI drawer has a rectangle while the surface is open");
    check(layout.rawLogOverlay.bottom() <= layout.composer.y + 0.5f,
          "and it ends at or above the composer's top edge - it never covers it");
    check(layout.thread.bottom() <= layout.rawLogOverlay.y + 0.5f,
          "the thread above it is shortened by exactly the drawer's height");
    check(layout.rawLogOverlay.h <= layout.rawLogBand.h + 0.5f,
          "and the drawer stays inside its band");
}

/// The surface's plain keys (review: `takeSurfaceKeys()` had no test). They are *characters*, not
/// key events (plan/key_gate.md F2), so they are injected the way a backend delivers them and read
/// through `ui::takeCharacter()` while the overlay owns the keyboard.
void checkPlainKeys() {
    tui::TuiState state = workbenchState();
    state.ui.rawLogOpen = true;
    state.logRecords.push_back(record(model::kRawLogLevelInfo, "a plain row", 0));
    shellFrame(state, 100, 30);
    check(state.overlayOwnsKeyboard(), "the overlay owns the keyboard while it is up");
    check(!ImGui::GetIO().WantTextInput, "and the composer no longer does");
    check(!state.ui.rawLogPaused, "the surface starts unpaused");

    g_w = 100;
    g_h = 30;
    frame(
        [&](const ui::Metrics&) {
            views::renderShell(state, ImVec2(static_cast<float>(g_w), static_cast<float>(g_h)));
        },
        "p");
    check(state.ui.rawLogPaused, "`p` pauses the follower");

    frame(
        [&](const ui::Metrics&) {
            views::renderShell(state, ImVec2(static_cast<float>(g_w), static_cast<float>(g_h)));
        },
        "f");
    check(!state.ui.rawLogPaused && state.ui.rawLogFollow, "`f` resumes it and re-arms follow");

    state.rawLog.copiedRows.clear();
    frame(
        [&](const ui::Metrics&) {
            views::renderShell(state, ImVec2(static_cast<float>(g_w), static_cast<float>(g_h)));
        },
        "y");
    check(contains(state.rawLog.copiedRows, "a plain row"), "`y` copies the visible rows");

    // `/` focuses the filter field: the surface sets `filterFocus` and hands it to the next
    // `SetKeyboardFocusHere()`, so the field is active from that same frame on.
    frame(
        [&](const ui::Metrics&) {
            views::renderShell(state, ImVec2(static_cast<float>(g_w), static_cast<float>(g_h)));
        },
        "/");
    frame([&](const ui::Metrics&) {
        views::renderShell(state, ImVec2(static_cast<float>(g_w), static_cast<float>(g_h)));
    });
    check(state.rawLog.filterFocused, "`/` hands the keyboard to the filter field");
    check(!state.rawLog.filterFocus, "and the request is spent once the field owns it");
    check(state.userQuery.inputBuf.empty(), "and nothing landed in the composer's draft");

    // Typing into the field must stay typing: `p` is the field's character, not the surface's key
    // (`ui::takeCharacter()` refuses while a field owns the keyboard).
    state.ui.rawLogPaused = false;
    frame(
        [&](const ui::Metrics&) {
            views::renderShell(state, ImVec2(static_cast<float>(g_w), static_cast<float>(g_h)));
        },
        "p");
    check(!state.ui.rawLogPaused, "`p` typed into the filter stays a character");
    check(std::string(state.rawLog.filter.data()).find('p') != std::string::npos,
          "and reaches the filter field");
}

/// The overlay stack's keyboard contract (review C1): only the topmost surface owns the
/// keyboard. The Raw Log draws before the palette; without the topmost check it steals the
/// palette's focus every frame, the palette closes on its focus-loss rule, and the Raw Log's
/// plain keys fire through it. The second half is the reverse direction: closing the Raw Log
/// under the palette must not hand the keyboard to the composer and dismiss the palette.
void checkOverlayStack() {
    tui::TuiState state = workbenchState();
    state.ui.prefs.rawLog = model::kRawLogTrace;
    for (int i = 0; i < 12; ++i) {
        state.logRecords.push_back(
            record(model::kRawLogLevelInfo, "stack " + std::to_string(i), i));
        ++state.logRecordSeq;
    }
    views::toggleRawLog(state);
    for (int i = 0; i < 3; ++i)
        overlayFrame(state, 100, 30);
    check(state.rawLog.open, "premise: the Raw Log overlay is open");
    check(state.rawLog.overlayShape, "premise: it is the text grid's modal shape");

    views::openPaletteForState(state.palette, state, "");
    for (int i = 0; i < 3; ++i)
        overlayFrame(state, 100, 30);
    check(state.palette.open, "the palette stays open above the Raw Log");
    check(ImGui::GetIO().WantTextInput, "and owns the keyboard");

    overlayFrame(state, 100, 30, "x");
    check(std::string(state.palette.query.data()) == "x",
          "typing lands in the palette query, not the Raw Log");
    overlayFrame(state, 100, 30, "p");
    check(!state.ui.rawLogPaused, "and the Raw Log's plain key does not fire through it");

    // Close the palette: the Raw Log is the next top surface and gets the keyboard back.
    views::closePalette(state.palette, state);
    for (int i = 0; i < 2; ++i)
        overlayFrame(state, 100, 30);
    check(!state.palette.open && state.rawLog.open, "the palette closes and the Raw Log stays");
    overlayFrame(state, 100, 30, "p");
    check(state.ui.rawLogPaused, "the Raw Log owns the keyboard again after the palette closes");

    // The reverse direction: a palette above the Raw Log survives closing the Raw Log under it.
    views::openPaletteForState(state.palette, state, "");
    for (int i = 0; i < 2; ++i)
        overlayFrame(state, 100, 30);
    views::toggleRawLog(state);
    for (int i = 0; i < 3; ++i)
        overlayFrame(state, 100, 30);
    check(state.palette.open, "closing the lower Raw Log does not dismiss the palette");
    check(!state.rawLog.open, "the Raw Log is closed");
}

void checkAbi() {
    check(tuiIsRawLogOpen(nullptr) == 0, "tuiIsRawLogOpen is null-safe");
    check(tuiRawLogFloor(nullptr) == 0, "tuiRawLogFloor is null-safe (trace)");
    tuiSetRawLogOpen(nullptr, 1); // must not crash
    check(true, "tuiSetRawLogOpen is null-safe");

    // The handle, not the C++ aggregate: this is the surface D links against, and the flag it
    // reports is the one D's frame poll turns into a logger level (`--debug-ui` sets it before
    // the first frame through exactly this call).
    TuiState* state = tuiCreateState();
    check(state != nullptr, "a state handle is created");
    check(tuiIsRawLogOpen(state) == 0, "a fresh state reports the surface closed");
    check(tuiRawLogFloor(state) == model::kRawLogInfo,
          "a fresh state reports the configured floor (info)");

    // The floor getter reads the preference itself, so each ladder word's severity round-trips
    // through the handle D polls: the raise condition is pref-valued, not a re-derived guess.
    for (const model::UiLogWord& word : model::kUiLogLadder) {
        state->inner->ui.prefs.rawLog = word.severity;
        check(tuiRawLogFloor(state) == word.severity,
              "the floor getter reports " + std::string(model::uiLogWord(word.severity)));
    }
    state->inner->ui.prefs.rawLog = model::kRawLogInfo;

    tuiSetRawLogOpen(state, 1);
    check(tuiIsRawLogOpen(state) == 1, "tuiSetRawLogOpen(1) opens it - the `--debug-ui` path");
    tuiSetRawLogOpen(state, 0);
    check(tuiIsRawLogOpen(state) == 0, "tuiSetRawLogOpen(0) closes it");
    // Idempotent: the flag is a state, not a toggle, so a repeated call cannot flip it.
    tuiSetRawLogOpen(state, 1);
    tuiSetRawLogOpen(state, 1);
    check(tuiIsRawLogOpen(state) == 1, "a repeated open is still open");
    tuiSetRawLogOpen(state, 0);
    check(tuiIsRawLogOpen(state) == 0, "and the close is honoured");
    tuiDestroyState(state);
}

void checkRememberedNothing() {
    // The open state is not a preference: `ui.conf` has no key for it, and the model's writer
    // never emits one (D-RAWLOG: "open state does not persist").
    model::ShellPrefs prefs;
    prefs.rawLog = model::kRawLogError;
    const std::string text = model::uiConfText(prefs);
    check(contains(text, "UiLog=error"), "the level is persisted");
    check(!contains(text, "RawLogOpen") && !contains(text, "rawLogOpen"), "the open state is not");
    check(!contains(text, "RawLogPaused") && !contains(text, "RawLogFollow"),
          "neither are the two view modes");
}

// 4b. The seq-gap accounting.

/// A record with a D-side stamp: `index = seq` keeps the clocks ordered with the stamps.
model::RawLogRecord stampedRecord(int level, const std::string& message, int seq) {
    model::RawLogRecord out = record(level, message, seq);
    out.seq = seq;
    return out;
}

/// The equation's remainder: `headN + interiorGapSum - dropped - queueDropNs`. Zero when every
/// missing seq was explained by a queue-drop warning; anything else is the diagnostic the
/// surfaces can print (a positive remainder is a loss the stamps prove but nothing explained).
std::int64_t remainderOf(const tui::TuiState& state) {
    const std::int64_t head =
        static_cast<std::int64_t>(model::rawLogHeadGap(state.logRecords, state.droppedRecords));
    const std::int64_t interior =
        static_cast<std::int64_t>(model::rawLogInteriorGapSum(state.logRecords));
    return head + interior - static_cast<std::int64_t>(state.droppedRecords) -
           static_cast<std::int64_t>(state.queueDropRecords);
}

void checkGapAccounting() {
    // A forced gap with no warning: the hole becomes a placeholder row, and the remainder is
    // the loss the stamps prove but nothing explained.
    tui::TuiState state = workbenchState();
    tui::tuiAddLogRecord(state, stampedRecord(model::kRawLogLevelInfo, "one", 1));
    tui::tuiAddLogRecord(state, stampedRecord(model::kRawLogLevelInfo, "two", 2));
    tui::tuiAddLogRecord(state, stampedRecord(model::kRawLogLevelInfo, "five", 5));
    check(state.logRecords.size() == 4, "a forced gap becomes a placeholder row");
    const model::RawLogRecord& placeholder = state.logRecords[2];
    check(placeholder.loss && placeholder.function == model::kRawLogSeqGapMarker,
          "the placeholder is a loss row with the gap marker");
    check(placeholder.message == model::rawLogGapText(2), "the placeholder names the hole");
    check(model::rawLogRowText(placeholder) == placeholder.message,
          "a loss row's text is its message alone");
    check(state.logRecords[0].message == "one" && state.logRecords[1].message == "two" &&
              state.logRecords[3].message == "five",
          "the real rows are untouched");
    check(state.lastStampedSeq == 5, "the watermark moved to the new stamp");
    check(remainderOf(state) == 2, "the unexplained gap is the remainder");

    // A queue-drop warning that explains exactly the gap: folded into one loss row, and the
    // equation balances.
    tui::TuiState folded = workbenchState();
    tui::tuiAddLogRecord(folded, stampedRecord(model::kRawLogLevelInfo, "one", 1));
    tui::tuiAddLogRecord(folded, stampedRecord(model::kRawLogLevelInfo, "two", 2));
    model::RawLogRecord warning =
        stampedRecord(model::kRawLogLevelWarn,
                      "log queue dropped 2 records (the frame loop did not drain in time)", 5);
    warning.function = model::kRawLogQueueDropMarker;
    tui::tuiAddLogRecord(folded, warning);
    check(folded.logRecords.size() == 3, "an explained warning folds into one loss row");
    const model::RawLogRecord& row = folded.logRecords.back();
    check(row.loss && row.message == model::rawLogQueueDropText(2),
          "the fold names the hole and its source");
    check(row.function == model::kRawLogQueueDropMarker && row.level == model::kRawLogLevelWarn,
          "the fold keeps the warning's level");
    check(folded.queueDropRecords == 2, "the warning's N is what the D-side sum counts");
    check(remainderOf(folded) == 0, "an explained gap balances");

    // A warning that claims a different N than the stamps prove: nothing folds, both rows
    // show, and the remainder is the discrepancy itself (positive: loss without an excuse).
    tui::TuiState mismatch = workbenchState();
    tui::tuiAddLogRecord(mismatch, stampedRecord(model::kRawLogLevelInfo, "one", 1));
    tui::tuiAddLogRecord(mismatch, stampedRecord(model::kRawLogLevelInfo, "two", 2));
    model::RawLogRecord liar =
        stampedRecord(model::kRawLogLevelWarn,
                      "log queue dropped 5 records (the frame loop did not drain in time)", 6);
    liar.function = model::kRawLogQueueDropMarker;
    tui::tuiAddLogRecord(mismatch, liar);
    check(mismatch.logRecords.size() == 4, "a miscounted warning folds nothing");
    check(mismatch.logRecords[2].message == model::rawLogGapText(3),
          "the hole shows at its own size");
    check(mismatch.logRecords[3].message == liar.message, "and the warning keeps its own wording");
    check(mismatch.queueDropRecords == 5, "the claimed N is summed as claimed");
    check(remainderOf(mismatch) == -2, "the discrepancy is the diagnostic");

    // A duplicate stamp: accepted as a normal record, no gap logic, and the watermark stays
    // put so the next in-order stamp does not look like a gap.
    tui::TuiState dup = workbenchState();
    tui::tuiAddLogRecord(dup, stampedRecord(model::kRawLogLevelInfo, "one", 1));
    tui::tuiAddLogRecord(dup, stampedRecord(model::kRawLogLevelInfo, "two", 2));
    tui::tuiAddLogRecord(dup, stampedRecord(model::kRawLogLevelInfo, "three", 3));
    tui::tuiAddLogRecord(dup, stampedRecord(model::kRawLogLevelInfo, "three again", 3));
    tui::tuiAddLogRecord(dup, stampedRecord(model::kRawLogLevelInfo, "four", 4));
    check(dup.logRecords.size() == 5, "a duplicate is accepted as a normal record");
    bool anyLoss = false;
    for (const model::RawLogRecord& r : dup.logRecords)
        anyLoss = anyLoss || r.loss;
    check(!anyLoss, "no placeholder follows a duplicate");
    check(dup.lastStampedSeq == 4, "the watermark did not move backwards");
    check(remainderOf(dup) == 0, "and the ring accounts clean");

    // Priming: a state created mid-process cannot know what D logged before it existed - the
    // first stamp is a starting point, not a hole.
    tui::TuiState primed = workbenchState();
    tui::tuiAddLogRecord(primed, stampedRecord(model::kRawLogLevelInfo, "late join", 500));
    check(primed.logRecords.size() == 1, "the first stamp is a starting point, not a hole");
    check(primed.lastStampedSeq == 500, "the watermark primed at the first stamp");
    check(remainderOf(primed) == 0, "and priming explains itself");

    // Unstamped records (seq 0) flow through unchanged and never start the watermark's gap
    // logic behind a number that is not a position.
    tui::TuiState legacy = workbenchState();
    tui::tuiAddLogRecord(legacy, record(model::kRawLogLevelInfo, "pre-stamp", 0));
    tui::tuiAddLogRecord(legacy, stampedRecord(model::kRawLogLevelInfo, "stamped", 1));
    check(legacy.logRecords.size() == 2 && !legacy.logRecords[0].loss,
          "an unstamped record flows through unchanged");
    check(legacy.lastStampedSeq == 1, "the watermark starts at the first stamp");
    check(remainderOf(legacy) == 0, "and no gap was invented");

    // The wrap: a placeholder the FIFO evicts is not counted as dropped - nothing arrived, so
    // losing the marker loses nothing. The fill runs far enough that the placeholder itself is
    // the third eviction (one, two, then the placeholder): the `!placeholder` branch is
    // exercised, and the two real records carry all of `droppedRecords`.
    tui::TuiState wrap = workbenchState();
    tui::tuiAddLogRecord(wrap, stampedRecord(model::kRawLogLevelInfo, "one", 1));
    tui::tuiAddLogRecord(wrap, stampedRecord(model::kRawLogLevelInfo, "two", 2));
    tui::tuiAddLogRecord(wrap, stampedRecord(model::kRawLogLevelInfo, "after gap", 5));
    for (int seq = 6; seq <= 5004; ++seq)
        tui::tuiAddLogRecord(wrap, stampedRecord(model::kRawLogLevelInfo, "fill", seq));
    check(wrap.logRecords.front().function != model::kRawLogSeqGapMarker,
          "premise: the placeholder itself was the eviction that just happened");
    check(wrap.droppedRecords == 2,
          "the placeholder's eviction is not a drop (the two real records are)");
    check(model::rawLogHeadGap(wrap.logRecords, wrap.droppedRecords) == 4,
          "the head gap counts the evicted and the never-arrived alike, placeholder evicted");
    check(model::rawLogInteriorGapSum(wrap.logRecords) == 0,
          "the interior gap moved into the head, not into both");
    check(remainderOf(wrap) == 2, "and the remainder is stable across the wrap");

    // An unstamped oldest survivor proves nothing came before it: even a wrapped ring
    // (dropped > 0) claims no head gap, because the stamps cannot answer for seqs the
    // store never saw stamped.
    tui::TuiState allUnstamped = workbenchState();
    for (int i = 0; i <= 5002; ++i)
        tui::tuiAddLogRecord(allUnstamped, record(model::kRawLogLevelInfo, "plain", i));
    check(allUnstamped.droppedRecords > 0, "premise: the all-unstamped ring wrapped");
    check(allUnstamped.logRecords.front().seq == 0, "premise: the oldest survivor is unstamped");
    check(model::rawLogHeadGap(allUnstamped.logRecords, allUnstamped.droppedRecords) == 0,
          "an unstamped oldest survivor claims no head gap");
    check(allUnstamped.queueDropRecords == 0,
          "and nothing is claimed for seqs no stamp ever backed");
}

// ---------------------------------------------------------------------------
// 5. The goldens
// ---------------------------------------------------------------------------

struct Scene {
    const char* name;
    const char* filter;
    bool paused;
    bool records;
    bool gap; // the loss scene: a wrapped ring with a placeholder
    int w, h;
};

std::vector<Scene> scenes() {
    return {
        {"rawlog-follow", "", false, true, false, 100, 30},
        {"rawlog-filtered", "read_file", false, true, false, 100, 30},
        {"rawlog-paused", "", true, true, false, 100, 30},
        {"rawlog-empty", "", false, false, false, 40, 15},
        {"rawlog-gap", "", false, true, true, 100, 30},
    };
}

/// The loss scene's ring content: stamps with the ring already wrapped (`dropped = 2`),
/// an interior placeholder marking the two seqs that never arrived, and no queue-drop warning -
/// the golden pins the head row, the placeholder row and their dim ink against the real rows.
std::deque<model::RawLogRecord> gapRecords() {
    std::deque<model::RawLogRecord> records;
    model::RawLogRecord first = record(model::kRawLogLevelInfo, "session opened", 0);
    first.seq = 3;
    records.push_back(first);
    model::RawLogRecord placeholder;
    placeholder.timestampMs = kFixtureEpochMs;
    placeholder.message = model::rawLogGapText(2);
    placeholder.function = model::kRawLogSeqGapMarker;
    placeholder.seq = 6; // the virtual slot: the seq of the record it precedes
    placeholder.loss = true;
    records.push_back(placeholder);
    model::RawLogRecord warned = record(model::kRawLogLevelWarn, "ui.conf is read-only", 2);
    warned.seq = 6;
    records.push_back(warned);
    model::RawLogRecord failed = record(model::kRawLogLevelError, "read_file failed", 3);
    failed.action = TuiLogAction_Retry;
    failed.seq = 7;
    records.push_back(failed);
    return records;
}

std::string goldenText(const Scene& scene) {
    g_w = scene.w;
    g_h = scene.h;
    const std::deque<model::RawLogRecord> records =
        scene.gap ? gapRecords()
                  : (scene.records ? mixedRecords() : std::deque<model::RawLogRecord>());
    const model::RawLogRing ring = ringOf(records, scene.gap ? 2u : 0u);
    const ui::Rect rect = surfaceRect(scene.w, scene.h);
    views::RawLogState state;
    state.open = true;
    state.overlayShape = true;
    state.minLevel = model::kRawLogLevelTrace;
    state.paused = scene.paused;
    if (scene.filter != nullptr && scene.filter[0] != '\0')
        std::snprintf(state.filter.data(), state.filter.size(), "%s", scene.filter);
    Grid grid;
    for (int i = 0; i < 2; ++i) {
        grid = frame([&](const ui::Metrics& m) { views::renderRawLog(state, rect, m, ring); });
    }

    std::string text = "name: ";
    text += scene.name;
    text += "\nscreen: " + std::to_string(scene.w) + "x" + std::to_string(scene.h);
    text += "\nsurface: x=0 y=0 w=" + std::to_string(scene.w) + " h=" + std::to_string(scene.h) +
            " overlay=1";
    text += "\nstate: follow=" + std::string(state.follow ? "1" : "0") +
            " paused=" + std::string(state.paused ? "1" : "0") + " filter='" +
            std::string(state.filter.data()) + "'";
    text += "\nparts: matched=" + std::to_string(state.matchingRecords) +
            " drawn=" + std::to_string(state.drawnRows) +
            " dropped=" + std::to_string(state.dropped);
    text += "\ntext:\n";
    for (int y = 0; y < grid.ny; ++y) {
        std::string row = grid.rowText(y);
        while (!row.empty() && row.back() == ' ')
            row.pop_back();
        text += row;
        text += '\n';
    }
    return text;
}

void checkGoldens(bool update) {
    const std::string dir = TEST_RAWLOG_GOLDEN_DIR;
    for (const Scene& scene : scenes()) {
        const std::string path = dir + "/" + scene.name + "-" + std::to_string(scene.w) + "x" +
                                 std::to_string(scene.h) + ".txt";
        const std::string text = goldenText(scene);
        if (update) {
            check(writeFile(path, text), "golden written: " + path);
            continue;
        }
        const std::string expected = readWhole(path);
        check(!expected.empty(),
              "golden is readable: " + path + " (run with --update to create it)");
        if (expected == text) {
            check(true, std::string(scene.name) + " " + std::to_string(scene.w) + "x" +
                            std::to_string(scene.h) + " golden matches");
            continue;
        }
        std::size_t line = 1;
        std::size_t at = 0;
        while (at < expected.size() && at < text.size() && expected[at] == text[at]) {
            if (expected[at] == '\n')
                ++line;
            ++at;
        }
        check(false, std::string(scene.name) + " " + std::to_string(scene.w) + "x" +
                         std::to_string(scene.h) + " golden matches (first difference on line " +
                         std::to_string(line) + ": " + path + ")");
        if (g_verbose)
            std::fprintf(stderr, "--- actual ---\n%s--- expected ---\n%s", text.c_str(),
                         expected.c_str());
    }
}

} // namespace

int main(int argc, char** argv) {
    bool update = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-v" || arg == "--verbose")
            g_verbose = true;
        else if (arg == "--update")
            update = true;
        else if (arg == "--print")
            g_print = true;
        else
            usage(argv[0], "unknown argument '" + arg + "'");
    }

    // Captures pin one glyph set and one clock whatever machine runs this: `LLMFUN_ASCII=0`
    // outranks TERM and the locale, and the row clock is local time, so the zone is pinned to
    // UTC before any capture (the Activity goldens' rule).
    ::setenv("LLMFUN_ASCII", "0", 1);
    ::setenv("TZ", "UTC", 1);
    ::tzset();

    ImGui::CreateContext();
    ImTui_ImplText_Init();

    checkInkCapture();
    checkSource();
    checkRowText();
    checkColumnPlan();
    checkWidePlanAndMaxima();
    checkDetailAndFilter();
    checkN2Identity();
    checkOpenableGlyph();
    checkRowInk();
    checkTagAndShortClock();
    checkLadder();
    checkPreview();
    checkSave();
    checkSurface();
    checkFilterAndStates();
    checkFilterHighlight();
    checkBoundsAndExpansionAndCopy();
    checkRingRotation();
    checkModelRules();
    checkDrawerResize();
    checkFollower();
    checkAccess();
    checkOverlayAndDrawer();
    checkPlainKeys();
    checkOverlayStack();
    checkAbi();
    checkRememberedNothing();
    checkGapAccounting();
    checkGoldens(update);

    ImGui::DestroyContext();

    if (g_failures == 0) {
        std::printf("test_rawlog: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "test_rawlog: %d of %d checks failed\n", g_failures, g_checks);
    return 1;
}
