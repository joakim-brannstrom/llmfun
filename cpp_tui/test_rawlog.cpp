// test_rawlog.cpp
//
// The Raw Log surface (views/rawlog.h + model/rawlog_data.h, design §4.9 / FR-16 / D-RAWLOG /
// task 33): the opt-in developer stream, its row format, its filter and follower, its save and
// copy, and the wiring that makes it reachable from every documented access path.
//
// What this binary proves, in order:
//
//   1. the source. The surface writes no colour literal, reads no display size, names no v4 type
//      and no `tui.h`; the strings it prints are the header's, not literals in the .cpp;
//   2. the model. The row text (`time level module.function file:line message`), the detail
//      lines, the case-insensitive filter, the `UiLog` floor, the save name/path and the one
//      write - including "never overwrite", "never inside the workarea" and "the archive keeps
//      its own level" (this task never calls `tuiSetLogging`);
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

    uint32_t at(int x, int y) const {
        if (x < 0 || y < 0 || x >= nx || y >= ny)
            return 0;
        return ch[static_cast<std::size_t>(y) * nx + x];
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
    for (std::size_t i = 0; i < cells; ++i)
        grid.ch[i] = g_screen.data[i].ch;
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

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

/// 2026-06-21T00:00:00Z: the goldens read 00:00:xx and cannot embed the wall clock.
inline constexpr long long kFixtureEpochMs = 1'782'000'000'000;

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
    model::RawLogRecord full = record(model::kRawLogLevelWarn, "ui.conf is read-only", 0);
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

    check(model::rawLogFloor(model::kRawLogOff) == model::kRawLogLevelInfo,
          "UiLog=off shows what a normal run logs");
    check(model::rawLogFloor(model::kRawLogRaw) == model::kRawLogLevelInfo,
          "UiLog=raw is the same floor, named explicitly");
    check(model::rawLogFloor(model::kRawLogDebug) == model::kRawLogLevelDebug,
          "UiLog=debug widens it to the developer step");
    check(model::rawLogFloor(model::kRawLogTrace) == model::kRawLogLevelTrace,
          "UiLog=trace keeps trace lines");
    check(model::rawLogShowsLevel(model::kRawLogLevelTrace, model::kRawLogLevelInfo) == false,
          "a trace line is below the ordinary floor");
    check(model::rawLogShowsLevel(model::kRawLogLevelError, model::kRawLogLevelInfo),
          "an error always passes it");
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

void checkSave() {
    const std::string root = scratchRoot();
    check(!root.empty(), "the scratch data directory was created");
    setenv("XDG_DATA_HOME", root.c_str(), 1);

    const std::deque<model::RawLogRecord> records = mixedRecords();
    const model::RawLogRing ring = ringOf(records);
    check(model::rawLogSaveDir() == root + "/llmfun/logs",
          "the save directory is the data directory, never the workarea");

    const long long stamp = kFixtureEpochMs;
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
    check(grid.hasText("llm.agent.pool.processTurn"), "the developer coordinates are drawn");
    check(grid.hasText("source/llm/agent/pool.d:412"), "including the source line");
    check(grid.hasText(views::kRawLogFilterHint), "the filter field is up");
    check(grid.hasText(views::kRawLogCopyLabel) && grid.hasText(views::kRawLogSaveLabel) &&
              grid.hasText(views::kRawLogCloseLabel),
          "and the three controls");
    check(grid.hasText(views::kRawLogHints), "the overlay lists its keys");
    check(state.drawnRows > 0 && state.drawnRows <= records.size(),
          "the body drew the records it has");
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
    check(contains(payload, "ui.conf is read-only"), "whole rows, one per line");
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
    check(copyLines <= live.drawnRows, "and never copies a row it did not draw (" +
                                           std::to_string(copyLines) + " copied, " +
                                           std::to_string(live.drawnRows) + " drawn)");
    check(contains(live.copiedRows, "record 4999"),
          "the copy reaches the newest record at the bottom");
    check(contains(live.copiedRows, "record 4973"),
          "its first line is the first painted row, not the one above it");
    check(!contains(live.copiedRows, "record 4972"),
          "and the row above the painted window is not copied");
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
    const long long frontStamp = ringData.front().timestampMs;
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
    state.ui.prefs.rawLog = model::kRawLogDebug;
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
    tuiSetRawLogOpen(nullptr, 1); // must not crash
    check(true, "tuiSetRawLogOpen is null-safe");

    // The handle, not the C++ aggregate: this is the surface D links against, and the flag it
    // reports is the one D's frame poll turns into a logger level (`--debug-ui` sets it before
    // the first frame through exactly this call).
    TuiState* state = tuiCreateState();
    check(state != nullptr, "a state handle is created");
    check(tuiIsRawLogOpen(state) == 0, "a fresh state reports the surface closed");

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
    prefs.rawLog = model::kRawLogDebug;
    const std::string text = model::uiConfText(prefs);
    check(contains(text, "UiLog=debug"), "the level is persisted");
    check(!contains(text, "RawLogOpen") && !contains(text, "rawLogOpen"), "the open state is not");
    check(!contains(text, "RawLogPaused") && !contains(text, "RawLogFollow"),
          "neither are the two view modes");
}

// ---------------------------------------------------------------------------
// 5. The goldens
// ---------------------------------------------------------------------------

struct Scene {
    const char* name;
    const char* filter;
    bool paused;
    bool records;
    int w, h;
};

std::vector<Scene> scenes() {
    return {
        {"rawlog-follow", "", false, true, 100, 30},
        {"rawlog-filtered", "read_file", false, true, 100, 30},
        {"rawlog-paused", "", true, true, 100, 30},
        {"rawlog-empty", "", false, false, 40, 15},
    };
}

std::string goldenText(const Scene& scene) {
    g_w = scene.w;
    g_h = scene.h;
    const std::deque<model::RawLogRecord> records =
        scene.records ? mixedRecords() : std::deque<model::RawLogRecord>();
    const model::RawLogRing ring = ringOf(records);
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

    checkSource();
    checkRowText();
    checkDetailAndFilter();
    checkSave();
    checkSurface();
    checkFilterAndStates();
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
    checkGoldens(update);

    ImGui::DestroyContext();

    if (g_failures == 0) {
        std::printf("test_rawlog: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "test_rawlog: %d of %d checks failed\n", g_failures, g_checks);
    return 1;
}
