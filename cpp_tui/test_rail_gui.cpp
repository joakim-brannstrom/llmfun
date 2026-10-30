// test_rail_gui.cpp
//
// The session rail in the pixel profile (task 23, design §4.3): the same view the
// text grid draws, rendered through the real GLFW/OpenGL3 backend at the design's
// 1280x820 window. The grid half of the rail lives in `./test_rail`; this binary
// exists for the three things only the pixel profile can answer:
//
//   1. the two-line threshold at 820 px - rows carry their preview line, and the
//      answer comes from `views::railRowLines()` (30 rows on the grid, 600 px here);
//   2. the rail is the docked column of the three-column tiling, it is opaque (its
//      surface is painted over the thread, not blended into it), and every row is
//      inside it;
//   3. a frame of the shipped workbench renders with no queued session action - the
//      rail's own hit targets and its character-level keys do not fire on their own.
//
// The screenshot (`rail-gui-1280x820.ppm`) is the visual evidence: the same frame the
// assertions measured.
//
// Needs a display (run under xvfb-run), exit codes: 0 = pass, 1 = a check failed,
// 2 = the backend could not start.
//
//   xvfb-run -s "-screen 0 1280x820x24" ./test_rail_gui [--out DIR]

#include "views/rail.h"

#include "tui.h"
#include "tui_api.h"
#include "tui_backend.h"
#include "tui_settings.h"
#include "ui/layout.h"
#include "ui/metrics.h"
#include "views/shell.h"

#include "imgui/imgui.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ui = llmfun::tui::ui;
namespace views = llmfun::tui::views;

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

/// A twelve-session snapshot: one pinned, and the rest spread over the date groups so
/// every header the rail can draw is on screen.
void seedSessions(llmfun::tui::TuiState& state, long long nowMs) {
    struct Fixture {
        const char* id;
        const char* title;
        const char* preview;
        int hoursAgo;
        bool pinned;
        bool active;
    };
    const Fixture fixtures[] = {
        {"s00", "Pinned: release checklist", "cut the build", 240, true, false},
        {"s01", "Session rail v2", "draw the visible window", 0, false, true},
        {"s02", "Composer paste handling", "bracketed paste", 2, false, false},
        {"s03", "Table bridge", "text grid renderer", 26, false, false},
        {"s04", "Streaming v2", "caret and elapsed", 30, false, false},
        {"s05", "Activity tab", "severity glyphs", 52, false, false},
        {"s06", "Inspector frame", "two tabs", 76, false, false},
        {"s07", "Palette completion", "every action", 100, false, false},
        {"s08", "Parity suite", "the merge gate", 150, false, false},
        {"s09", "Cutover", "flag-based rollback", 200, false, false},
        {"s10", "Help overlay", "question mark", 260, false, false},
        {"s11", "Focus mode", "one region", 400, false, false},
    };
    for (const Fixture& f : fixtures) {
        llmfun::tui::SessionEntry entry;
        entry.id = f.id;
        entry.title = f.title;
        entry.preview = f.preview;
        entry.messageCount = 3;
        entry.isActive = f.active;
        entry.updatedAtMs = nowMs - static_cast<long long>(f.hoursAgo) * 3'600'000LL;
        state.sessionPanel.sessions.push_back(entry);
        if (f.active)
            state.sessionPanel.activeId = f.id;
        if (f.pinned)
            state.ui.prefs.pinned.insert(f.id);
    }
    // One non-active row carries the run flag (`SessionItem::flags` bit 0, task 24):
    // the frame then has a spinner to draw, and the checks below can pin the single
    // hop from the ABI field to the rail's own row. `s02` is the third row, so it is
    // inside the drawn window at 1280x820 whatever the group headers cost.
    for (auto& entry : state.sessionPanel.sessions) {
        if (entry.id == "s02")
            entry.flags = llmfun::tui::kSessionFlagRunning;
    }
}

/// One P6 capture. The GUI has no cell grid to read (that half of the rail lives in
/// `./test_rail`), so a claim about what a row *looks like* has to be a claim about
/// the frame the backend produced - the same seam `test_palette` uses.
struct Ppm {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgb;
};

bool readPpm(const std::string& path, Ppm& out) {
    FILE* in = std::fopen(path.c_str(), "rb");
    if (in == nullptr)
        return false;
    char magic[3] = {0, 0, 0};
    int width = 0;
    int height = 0;
    int maxValue = 0;
    const bool header = std::fscanf(in, "%2s %d %d %d", magic, &width, &height, &maxValue) == 4;
    const int separator = header ? std::fgetc(in) : EOF;
    if (!header || std::strcmp(magic, "P6") != 0 || width <= 0 || height <= 0 || maxValue != 255 ||
        separator == EOF) {
        std::fclose(in);
        return false;
    }
    out.width = width;
    out.height = height;
    out.rgb.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);
    const std::size_t read = std::fread(out.rgb.data(), 1, out.rgb.size(), in);
    std::fclose(in);
    return read == out.rgb.size();
}

/// Where two captures disagree: the bounding box of the differing pixels and how many
/// there are. `count == 0` means the frames are identical, and `y1 < y0` is the empty
/// box of that case, so `y1 - y0 + 1` is always the height of the change.
struct DiffBox {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = -1;
    int count = 0;
};

DiffBox diffBox(const Ppm& a, const Ppm& b) {
    DiffBox box;
    if (a.width != b.width || a.height != b.height)
        return box;
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * a.width + x) * 3;
            if (a.rgb[i] == b.rgb[i] && a.rgb[i + 1] == b.rgb[i + 1] &&
                a.rgb[i + 2] == b.rgb[i + 2])
                continue;
            if (box.count == 0) {
                box.x0 = box.x1 = x;
                box.y0 = box.y1 = y;
            } else {
                box.x0 = x < box.x0 ? x : box.x0;
                box.x1 = x > box.x1 ? x : box.x1;
                box.y0 = y < box.y0 ? y : box.y0;
                box.y1 = y > box.y1 ? y : box.y1;
            }
            ++box.count;
        }
    }
    return box;
}

/// Write the run flag on one snapshot row (the ABI's `SessionItem::flags`, bit 0).
void setRunningFlag(llmfun::tui::TuiState& state, const std::string& id, bool running) {
    for (auto& entry : state.sessionPanel.sessions) {
        if (entry.id == id)
            entry.flags = running ? llmfun::tui::kSessionFlagRunning : 0;
    }
}

/// Write the timestamp on one snapshot row (`0` = unknown, which flattens the rail).
void setTimestamp(llmfun::tui::TuiState& state, const std::string& id, long long updatedAtMs) {
    for (auto& entry : state.sessionPanel.sessions) {
        if (entry.id == id)
            entry.updatedAtMs = updatedAtMs;
    }
}

/// Write the title on one snapshot row. A short title is how the checks below keep
/// the label out of the measurement: a title whose room shrinks by one glyph would
/// truncate, and its re-ellipsis would show up as pixels of its own.
void setTitle(llmfun::tui::TuiState& state, const std::string& id, const std::string& title) {
    for (auto& entry : state.sessionPanel.sessions) {
        if (entry.id == id)
            entry.title = title;
    }
}

} // namespace

int main(int argc, char** argv) {
    using namespace llmfun::tui;

    std::string outDir = ".";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc)
            outDir = argv[++i];
        else if (std::strcmp(argv[i], "-v") == 0)
            g_verbose = true;
        else {
            std::fprintf(stderr, "usage: %s [-v] [--out DIR]\n", argv[0]);
            return 2;
        }
    }

    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;

    llmfun::tui::TuiState state;
    state.readyStatus = true;
    const long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
    seedSessions(state, nowMs);

    TuiSettings view;
    view.fontScaleMain = 1.0f;
    view.windowWidth = 1280;
    view.windowHeight = 820;
    const std::string ini = outDir + "/rail-gui.conf";
    saveTuiSettings(view, ini);

    std::string error;
    state.backend = makeGuiBackend(ini, state.settings);
    if (!state.backend->init(error)) {
        std::fprintf(stderr, "GUI backend init failed (no display?): %s\n", error.c_str());
        ImGui::DestroyContext();
        return 2;
    }
    initMarkdownGuiMode(state);
    applyUiTheme();

    const ImVec2 display(1280.0f, 820.0f);
    const ui::Metrics m = ui::metricsFor();
    check(!m.textGrid, "premise: the pixel profile is live");
    check(views::railRowLines(m, display.y) == 2,
          "820 px gives the two-line row (the design's >= 600 px rule)");

    const ui::LayoutPrefs prefs = views::layoutPrefsFor(state.ui, state.palette.open);
    const ui::ShellLayout layout = views::shellLayoutFor(prefs, display);
    check(layout.rail.w > 0.0f && !layout.railOverlay,
          "the rail is the docked left column at 1280x820 (w=" + std::to_string(layout.rail.w) +
              ")");

    // The frame the screenshot is taken from: the shipped shell, the shipped renderer.
    const std::string shot = outDir + "/rail-gui-1280x820.ppm";
    if (!state.backend->captureNextFramePpm(shot)) {
        std::fprintf(stderr, "the GUI backend refused the capture request\n");
        std::exit(2);
    }
    state.backend->newFrame();
    tuiRender(state);
    state.backend->renderFrame();

    check(state.ui.rail.rowLines == 2,
          "the rail drew two-line rows (rowLines=" + std::to_string(state.ui.rail.rowLines) + ")");
    // Twelve sessions plus five group headers do not fit a 694 px column at 48 px a
    // row, so the rail draws the rows that fit and leaves the rest to the scrollbar -
    // that bound is the point, not a shortfall.
    const std::size_t firstDrawn = state.ui.rail.drawnRows;
    check(firstDrawn > 0 && firstDrawn < state.sessionPanel.sessions.size(),
          "the rail draws the rows that fit (" + std::to_string(firstDrawn) + " of " +
              std::to_string(state.sessionPanel.sessions.size()) + ")");
    check(state.sessionPanel.actions.empty(),
          "a rendered frame queues no session action by itself");
    check(tuiIsSessionActionReady(nullptr) == 0, "the C API reports an empty queue as well");

    // A second frame: the capture is one-shot, so the first one is already consumed and
    // the rail must not change its answer without input.
    state.backend->newFrame();
    tuiRender(state);
    state.backend->renderFrame();
    check(state.ui.rail.rowLines == 2 && state.ui.rail.drawnRows == firstDrawn,
          "the rail is stable across frames (no state drift)");

    // --- The scrollbar must not cover the time column ----------------------
    // The rows child overflows (12 sessions + 5 group headers), so ImGui draws its
    // vertical bar from the second frame on. Capture that frame and measure the
    // rightmost ink of the first session row: a row sized to the rail's *outer*
    // width puts the last glyph of the right-aligned time under the bar (the
    // pixel-profile "10m shows 1" report). The bar's own column is excluded.
    {
        const std::string barShot = outDir + "/rail-gui-scrollbar.ppm";
        check(state.backend->captureNextFramePpm(barShot), "the scrollbar frame is capturable");
        state.backend->newFrame();
        tuiRender(state);
        state.backend->renderFrame();
        Ppm bar;
        check(readPpm(barShot, bar), "the scrollbar frame was captured");
        const float barX = layout.rail.x + layout.rail.w - ImGui::GetStyle().ScrollbarSize;
        const int y0 = static_cast<int>(layout.rail.y + 2.0f * m.row + m.border);
        const int y1 = y0 + static_cast<int>(2.0f * m.row);
        const int x0 = static_cast<int>(layout.rail.x + ImGui::GetStyle().WindowPadding.x);
        const int xEnd = static_cast<int>(barX) - 1;
        const std::size_t bg = (static_cast<std::size_t>(y0 + static_cast<int>(m.row)) * bar.width +
                                static_cast<std::size_t>(x0 + 2)) *
                               3;
        int rightmost = x0 - 1;
        if (bar.width > 0 && bg + 2 < bar.rgb.size()) {
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x <= xEnd; ++x) {
                    const std::size_t i = (static_cast<std::size_t>(y) * bar.width + x) * 3;
                    if (bar.rgb[i] != bar.rgb[bg] || bar.rgb[i + 1] != bar.rgb[bg + 1] ||
                        bar.rgb[i + 2] != bar.rgb[bg + 2]) {
                        if (x > rightmost)
                            rightmost = x;
                    }
                }
            }
        }
        check(rightmost <= static_cast<int>(barX) - static_cast<int>(m.pad1),
              "the time column ends left of the scrollbar (ink " + std::to_string(rightmost) +
                  ", bar " + std::to_string(static_cast<int>(barX)) + ")");
    }

    // ---------------------------------------------------------------------
    // The shell bridge: `deriveRail()` + `applyRailAnswers()`
    // ---------------------------------------------------------------------
    // The rail's answers only become D actions here, so this is where the session
    // protocol is pinned: one Select queues one action, a switch made while a run is
    // active is deferred and flushed on the first ready frame, new/rename/delete are
    // refused while a run is active, a pin edits `ShellPrefs`, and a select clears the
    // unread dot. Every step below is one real frame of the shipped workbench.
    const auto frameOnce = [&]() {
        state.backend->newFrame();
        tuiRender(state);
        state.backend->renderFrame();
    };
    const auto clearActions = [&state]() { state.sessionPanel.actions.clear(); };

    // (a) a select while ready: one action, and the loading state that draws the
    //     pending mark (the skeleton only when the list is empty).
    state.ui.rail.selectId = "s03";
    frameOnce();
    check(state.sessionPanel.actions.size() == 1, "a select queues exactly one action");
    check(!state.sessionPanel.actions.empty() &&
              state.sessionPanel.actions.back().type == llmfun::tui::SessionActionType::Select &&
              state.sessionPanel.actions.back().sessionId == "s03",
          "the queued action is Select(s03)");
    check(state.ui.rail.selectId.empty(), "...and the answer is cleared after it is applied");
    check(state.ui.rail.loadingId == "s03", "the switch is marked as sent and unacknowledged");
    clearActions();

    // (b) the snapshot acknowledges it.
    state.sessionPanel.activeId = "s03";
    frameOnce();
    check(state.ui.rail.loadingId.empty(), "a snapshot naming that session clears the wait");
    clearActions();

    // (c) a select while a run is active is deferred, not dropped.
    state.readyStatus = false;
    state.ui.rail.selectId = "s04";
    frameOnce();
    check(state.sessionPanel.actions.empty(), "a select during a run queues nothing yet");
    check(state.ui.rail.pendingId == "s04", "...it is held in the single deferred slot");

    // (d) the first ready frame flushes it as an ordinary Select.
    state.readyStatus = true;
    frameOnce();
    check(state.sessionPanel.actions.size() == 1 &&
              state.sessionPanel.actions.back().sessionId == "s04",
          "the deferred switch is flushed on the first ready frame");
    check(state.ui.rail.pendingId.empty() && state.ui.rail.loadingId == "s04",
          "...and becomes a sent-and-unacknowledged switch");
    clearActions();

    // (e) new / rename / delete are refused while a run is active (D would reject
    //     them), and accepted when it is not.
    state.readyStatus = false;
    state.ui.rail.newRequested = true;
    state.ui.rail.renameId = "s05";
    state.ui.rail.renamedTitle = "Renamed while busy";
    state.ui.rail.deleteId = "s06";
    frameOnce();
    check(state.sessionPanel.actions.empty(), "no new/rename/delete is queued during a run");
    check(state.ui.rail.renameId.empty() && state.ui.rail.deleteId.empty() &&
              !state.ui.rail.newRequested,
          "...and each refused answer is consumed instead of replayed next frame");
    state.readyStatus = true;
    clearActions();
    state.ui.rail.renameId = "s05";
    state.ui.rail.renamedTitle = "Renamed";
    state.ui.rail.deleteId = "s06";
    frameOnce();
    bool sawRename = false;
    bool sawDelete = false;
    for (const auto& action : state.sessionPanel.actions) {
        if (action.type == llmfun::tui::SessionActionType::Rename && action.sessionId == "s05" &&
            action.title == "Renamed")
            sawRename = true;
        if (action.type == llmfun::tui::SessionActionType::Delete && action.sessionId == "s06")
            sawDelete = true;
    }
    check(sawRename, "a committed rename is queued as Rename(id, title)");
    check(sawDelete, "a delete answer is queued as Delete(id)");
    clearActions();

    // (f) a pin edits the preference (the one owner), both ways.
    state.ui.rail.pinToggleId = "s07";
    frameOnce();
    check(state.ui.prefs.pinned.count("s07") != 0, "`p` pins the session in ShellPrefs");
    state.ui.rail.pinToggleId = "s07";
    frameOnce();
    check(state.ui.prefs.pinned.count("s07") == 0, "`p` again unpins it");

    // (g) selecting a session clears its unread dot.
    std::vector<llmfun::tui::model::SessionRow> trackerRows;
    for (const auto& entry : state.sessionPanel.sessions) {
        llmfun::tui::model::SessionRow row;
        row.id = entry.id;
        row.title = entry.title;
        row.updatedAtMs = entry.updatedAtMs;
        row.active = entry.id == state.sessionPanel.activeId;
        trackerRows.push_back(row);
    }
    state.ui.unread.apply(trackerRows);
    for (auto& row : trackerRows)
        row.updatedAtMs += 60'000; // every session moved on
    state.ui.unread.apply(trackerRows);
    check(state.ui.unread.isUnread("s08"), "premise: an advanced session is unread");
    state.ui.rail.selectId = "s08";
    frameOnce();
    check(!state.ui.unread.isUnread("s08"), "selecting a session clears its dot");
    clearActions();

    // (h) the derivation is not tied to drawing: with the rail collapsed the deferred
    //     switch still flushes (a user who presses Ctrl+B mid-switch is not stranded).
    state.ui.prefs.railOpen = 0;
    state.readyStatus = false;
    state.ui.rail.selectId = "s09";
    frameOnce();
    check(state.ui.rail.pendingId == "s09", "premise: the switch is deferred while collapsed");
    state.readyStatus = true;
    frameOnce();
    check(state.sessionPanel.actions.size() == 1 &&
              state.sessionPanel.actions.back().sessionId == "s09",
          "a collapsed rail still flushes the switch it queued");
    state.ui.prefs.railOpen = -1;
    clearActions();

    // (i) "show me the search": the collapsed shape is the width rule, so the answer is a
    //     width request and not a write to the open flag. At 1280 px the rail already has
    //     its column, so this pins the wiring (`focusSearch` -> `UiState::railW`); the
    //     band where the request actually widens the column is pinned in
    //     `./test_shell_frame`.
    check(state.ui.railW == 0.0f, "premise: no rail width has been requested yet");
    state.ui.rail.focusSearch = true;
    frameOnce();
    check(state.ui.railW == ui::metricsFor().railW,
          "the search request asks for the profile's rail width (railW=" +
              std::to_string(state.ui.railW) + ")");
    check(state.ui.rail.filterFocus,
          "...and the search field is asked for focus on the following frame");
    frameOnce();
    check(!state.ui.rail.filterFocus,
          "...which consumes it: the field exists and took the keyboard");
    check(!state.ui.rail.searchEnded,
          "...and the search stays open while the field holds the keyboard");

    // The mode ends where it started: the view reports it and the frame hands the width
    // back. The widened column is borrowed for the search, not a new session default -
    // otherwise a narrow display would keep it until the process restarted.
    check(state.ui.rail.searchWide, "premise: the widen is owned by the search");
    state.ui.rail.searchEnded = true;
    frameOnce();
    check(!state.ui.rail.searchWide && state.ui.railW == 0.0f,
          "the end of the search hands the borrowed width back (railW=" +
              std::to_string(state.ui.railW) + ")");

    // ...and so does closing the rail under it (`Ctrl+B`): the shape the user comes back
    // to is the one the width breakpoint answers, not the widened column.
    state.ui.rail.focusSearch = true;
    frameOnce(); // opens and widens
    frameOnce(); // the field takes the keyboard
    check(state.ui.railW == ui::metricsFor().railW, "premise: the search widened the rail again");
    state.ui.prefs.railOpen = 0;
    frameOnce();
    check(!state.ui.rail.searchWide && state.ui.railW == 0.0f,
          "closing the rail drops the width the search borrowed");
    state.ui.prefs.railOpen = -1;
    frameOnce();

    // (j) a second select starts its own budget: the bound measures the wait it is
    //     bounding, not how long the previous one had been running.
    state.ui.rail.loadingId = "s05";
    state.ui.rail.loadingFrames = 100;
    state.ui.rail.selectId = "s06";
    frameOnce();
    check(state.ui.rail.loadingId == "s06" && state.ui.rail.loadingFrames == 0,
          "a new switch starts a fresh loading budget (frames=" +
              std::to_string(state.ui.rail.loadingFrames) + ", inherited 100)");
    clearActions();

    // (k) the run flag reaches the rail. `deriveRail()` is the *only* reader of
    //     `SessionItem::flags` (bit 0 = running), and the rail draws a running row with
    //     the spinner in place of its time label. Nothing else in the suite crosses that
    //     hop - `./test_rail` sets `SessionRow::running` directly - so a wrong bit, an
    //     inverted test or a deleted line would pass every gate with the field carried,
    //     unit-tested at the ABI boundary and visually dead. The GUI has no cells to
    //     read, so the assertion is the pair of frames: identical pixels for an
    //     unchanged snapshot, one local change inside the row's band for the flag.
    const auto captureFrame = [&](const std::string& name) {
        Ppm shot;
        const std::string path = outDir + "/" + name;
        if (!state.backend->captureNextFramePpm(path)) {
            check(false, "the GUI backend refused the capture request: " + path);
            return shot;
        }
        frameOnce();
        if (!readPpm(path, shot))
            check(false, "the capture is not a readable P6 PPM: " + path);
        return shot;
    };

    const ui::LayoutPrefs railPrefs = views::layoutPrefsFor(state.ui, state.palette.open);
    const ui::ShellLayout railLayout = views::shellLayoutFor(railPrefs, display);
    const float rowHeight = ui::metricsFor().row;

    setRunningFlag(state, "s02", true);
    const Ppm runningFrame = captureFrame("rail-running.ppm");
    setRunningFlag(state, "s02", false);
    const Ppm idleFrame = captureFrame("rail-running-cleared.ppm");
    const Ppm idleAgain = captureFrame("rail-running-idle-again.ppm");

    const DiffBox idleDiff = diffBox(idleFrame, idleAgain);
    check(idleDiff.count == 0,
          "premise: an unchanged snapshot renders the same pixels (differing=" +
              std::to_string(idleDiff.count) + ")");
    const DiffBox runDiff = diffBox(runningFrame, idleFrame);
    check(runDiff.count > 0, "the run flag changes what the rail draws (differing=" +
                                 std::to_string(runDiff.count) + ")");
    check(runDiff.x0 >= static_cast<int>(railLayout.rail.x) &&
              runDiff.x1 <= static_cast<int>(railLayout.rail.x + railLayout.rail.w),
          "...inside the rail column only (x=" + std::to_string(runDiff.x0) + ".." +
              std::to_string(runDiff.x1) + ")");
    check(static_cast<float>(runDiff.y1 - runDiff.y0 + 1) <= 3.0f * rowHeight,
          "...within one row's band, where the spinner replaces that row's time label (h=" +
              std::to_string(runDiff.y1 - runDiff.y0 + 1) + " px)");
    check(static_cast<float>(runDiff.x1) > railLayout.rail.x + 0.5f * railLayout.rail.w,
          "...and on the row's right-hand side, where the time label was drawn");

    // The same pair with no timestamp on that row: `updatedAtMs == 0` flattens the whole
    // rail (the model never fabricates a date), so the two frames share that shape and
    // only the flag differs. The title is shortened at the same time so the label cannot
    // re-ellipsize: with nothing in the time column, the only thing the flag can change is
    // the mark itself - which is what "a running row with no time still says why it is
    // quiet" means, and what separates the mark from a vanished label in the pair above.
    setTimestamp(state, "s02", 0);
    setTitle(state, "s02", "Run");
    setRunningFlag(state, "s02", true);
    frameOnce(); // the snapshot changed shape (flat list), so let the frame settle first
    const Ppm runningNoTime = captureFrame("rail-running-notime.ppm");
    setRunningFlag(state, "s02", false);
    const Ppm idleNoTime = captureFrame("rail-running-notime-cleared.ppm");
    const DiffBox noTimeDiff = diffBox(runningNoTime, idleNoTime);
    check(noTimeDiff.count > 0,
          "with no timestamp the flag alone still draws its mark (differing=" +
              std::to_string(noTimeDiff.count) + ")");
    check(noTimeDiff.x0 > static_cast<int>(railLayout.rail.x + 0.5f * railLayout.rail.w),
          "...entirely in the right-hand slot the time label uses (x=" +
              std::to_string(noTimeDiff.x0) + ".." + std::to_string(noTimeDiff.x1) +
              ", y=" + std::to_string(noTimeDiff.y0) + ".." + std::to_string(noTimeDiff.y1) + ")");

    state.backend->shutdown();
    ImGui::DestroyContext();

    std::printf("%s: %d check(s), %d failure(s); wrote %s\n", g_failures == 0 ? "PASS" : "FAIL",
                g_checks, g_failures, shot.c_str());
    return g_failures == 0 ? 0 : 1;
}
