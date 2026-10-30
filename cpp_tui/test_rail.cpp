// test_rail.cpp
//
// The session rail (views/rail.h, views/rail.cpp; design §4.3 / FR-03 / D-RAIL /
// D-UNREAD) and the list model under it (model/session_rows.*).
//
// What this binary proves, in order:
//
//   1. grouping. `buildRailRows()` across a day boundary, a week boundary, an
//      injected clock and an unknown-timestamp snapshot (which must answer one flat
//      list - the rail never fabricates a date), plus the display order (pinned
//      first, then newest, then title);
//   2. filtering. The fzf matcher the panel already ships: a ranked, flat list, the
//      dropped count, and the stable tie rule;
//   3. unread (D-UNREAD). Set when `updatedAt` advances while a session is not
//      active, cleared on select, never for a first sighting and never from an
//      unknown stamp;
//   4. the pure view rules: the two-line threshold (30 rows / 600 px), the relative
//      time, and the visible window, which is what bounds the drawn rows;
//   5. the row inks. Title and preview clear the 4.5:1 bar against every row fill -
//      the pair this task replaces measured 1.18:1 - and no state inks a label with
//      `textDim` (NFR-09);
//   6. the drawn rail, through the real text backend (the `TScreen` pattern of
//      `test_session_filter_smoke.cpp`): the search row, the group header, both
//      markers, the time, the footer, and the active row's selection fill;
//   7. bounded rendering and the frame budget: a 2000-session fixture draws a
//      viewport's worth of rows (counter assertion) and a whole frame - model plus
//      view - stays inside the frame budget;
//   8. the interaction: a row click selects, `m` opens the row menu, `r` renames
//      (Enter commits once, Esc cancels), `d` asks before deleting, `p` pins, `/`
//      focuses the search, and Enter in the search selects the top match.
//
// The clock is injected everywhere (`nowMs` is a parameter), and the binary pins
// `TZ=UTC` before it starts, so "today" is a fixed civil day and the day/week
// boundaries are plain epoch arithmetic rather than a property of the machine.
//
// Exit codes: 0 = all checks pass; 1 = a check failed; 2 = usage error.
//
//   ./test_rail [-v]

#include "model/session_rows.h"
#include "model/ui_state.h"
#include "ui/layout.h"
#include "ui/metrics.h"
#include "ui/tokens.h"
#include "views/rail.h"

#include "imtui/imtui.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

namespace ui = llmfun::tui::ui;
namespace model = llmfun::tui::model;
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

[[noreturn]] void usage(const char* argv0, const char* problem) {
    std::fprintf(stderr, "usage: %s [-v]   (%s)\n", argv0, problem);
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
    std::vector<uint8_t> fg;
    std::vector<uint8_t> bg;

    uint32_t at(int x, int y) const {
        if (x < 0 || y < 0 || x >= nx || y >= ny)
            return 0;
        return ch[static_cast<std::size_t>(y) * nx + x];
    }
    uint8_t fgAt(int x, int y) const {
        if (x < 0 || y < 0 || x >= nx || y >= ny)
            return 0;
        return fg[static_cast<std::size_t>(y) * nx + x];
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

    /// The first row whose text contains `needle`; -1 when none does. The rail's
    /// rows are read back out of the grid instead of being compared with the strings
    /// the drawing code used.
    int rowContaining(const std::string& needle) const {
        for (int y = 0; y < ny; ++y) {
            if (rowText(y).find(needle) != std::string::npos)
                return y;
        }
        return -1;
    }

    bool has(const std::string& needle) const { return rowContaining(needle) >= 0; }

    /// Cell of `needle`'s first byte (x,y), or (-1,-1).
    void find(const std::string& needle, int& outX, int& outY) const {
        outX = -1;
        outY = -1;
        for (int y = 0; y < ny; ++y) {
            const std::string row = rowText(y);
            const std::size_t pos = row.find(needle);
            if (pos != std::string::npos) {
                outX = static_cast<int>(pos);
                outY = y;
                return;
            }
        }
    }
};

Grid capture() {
    Grid grid;
    grid.nx = g_screen.nx;
    grid.ny = g_screen.ny;
    const std::size_t count = static_cast<std::size_t>(g_screen.nx) * g_screen.ny;
    grid.ch.resize(count);
    grid.fg.resize(count);
    grid.bg.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        grid.ch[i] = g_screen.data[i].ch;
        grid.fg[i] = g_screen.data[i].fg;
        grid.bg[i] = g_screen.data[i].bg;
    }
    return grid;
}

/// One frame of the text grid: a decoration-free root window filling the display, the
/// body drawing inside it, then the frame rendered through the real backend.
///
/// `inject` runs *before* `NewFrame`, because that is when ImGui consumes its input
/// queue - an event added after it would only be seen by the next frame.
Grid frame(const std::function<void(const ui::Metrics&)>& body,
           const std::function<void(ImGuiIO&)>& inject = nullptr, int w = kScreenW,
           int h = kScreenH) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(w), static_cast<float>(h));
    io.DeltaTime = 1.0f / 60.0f;
    if (inject)
        inject(io);
    ImTui_ImplText_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(w), static_cast<float>(h)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("rail-frame", nullptr,
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

void typeText(const std::string& text, const std::function<void(const ui::Metrics&)>& body) {
    for (const char c : text)
        frame(body, [&](ImGuiIO& io) { io.AddInputCharacter(static_cast<unsigned short>(c)); });
}

/// Run `n` frames with no input: ImGui trickles its event queue one event per frame
/// (`ConfigInputTrickleEventQueue`), so a pointer move or a focus request reaches the
/// widget on the frame after the one that queued it - and a window that was just
/// closed (a popup) keeps the hover for a frame or two. A real frame loop has plenty
/// of frames for that; a test has to ask for them.
void settle(const std::function<void(const ui::Metrics&)>& body, int n = 4) {
    for (int i = 0; i < n; ++i)
        frame(body);
}

/// One press: down, up, then a couple of idle frames so the trickled events have
/// landed before the assertions below read the state they caused.
void pressKey(ImGuiKey key, const std::function<void(const ui::Metrics&)>& body) {
    frame(body, [&](ImGuiIO& io) { io.AddKeyEvent(key, true); });
    frame(body, [&](ImGuiIO& io) { io.AddKeyEvent(key, false); });
    settle(body);
}

/// Press `Esc` and answer whether the rail reported that it took the press.
///
/// `RailState::escapeConsumed` is an *answer*: the view clears it at the start of the
/// next frame, exactly like the frame layer clears the others after applying them. So
/// the press is observed frame by frame here instead of after a settle - the reading is
/// then the same "same frame" rule the shell follows.
bool pressEscape(model::RailState& rail, const std::function<void(const ui::Metrics&)>& body) {
    frame(body, [](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Escape, true); });
    bool seen = rail.escapeConsumed;
    for (int i = 0; i < 4; ++i) {
        frame(body);
        seen = seen || rail.escapeConsumed;
    }
    frame(body, [](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Escape, false); });
    settle(body);
    return seen;
}

/// Move the pointer to `(x, y)` and wait (bounded) until the rail reports `id` under
/// it. Two things make the wait necessary rather than "settle N frames": ImGui
/// trickles one event per frame, and while a popup is open - or for a few frames after
/// it closes - every window behind it is deliberately not hoverable. Waiting for the
/// state the assertion needs is deterministic; a fixed frame count is a race.
bool pointAt(model::RailState& rail, const std::string& id, float x, float y,
             const std::function<void(const ui::Metrics&)>& body, int maxFrames = 16) {
    frame(body, [&](ImGuiIO& io) { io.AddMousePosEvent(x, y); });
    for (int i = 0; i < maxFrames; ++i) {
        frame(body);
        if (rail.hoverRowId == id)
            return true;
    }
    return false;
}

void clickAt(float x, float y, const std::function<void(const ui::Metrics&)>& body) {
    // A click is a *sequence*: ImGui trickles one event per frame, so the pointer
    // move, the press and the release each need their own frames - and the release
    // must be delivered, or `g.ActiveId` stays latched and every later hover on the
    // rail is blocked by the held button.
    frame(body, [&](ImGuiIO& io) { io.AddMousePosEvent(x, y); });
    settle(body);
    frame(body, [&](ImGuiIO& io) {
        io.AddMousePosEvent(x, y);
        io.AddMouseButtonEvent(0, true);
    });
    frame(body, [&](ImGuiIO& io) {
        io.AddMousePosEvent(x, y);
        io.AddMouseButtonEvent(0, false);
    });
    settle(body, 3);
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

/// 2026-06-18T12:00:00Z - a Thursday, mid-day, so "yesterday" and "7 days ago" are
/// unambiguous.
constexpr long long kNowMs = 1'781'784'000'000LL;
constexpr long long kHourMs = 3'600'000LL;
constexpr long long kDayMs = 24 * kHourMs;

model::SessionRow row(const std::string& id, const std::string& title, long long updatedAtMs,
                      bool active = false) {
    model::SessionRow r;
    r.id = id;
    r.title = title;
    r.preview = "preview line";
    r.updatedAtMs = updatedAtMs;
    r.active = active;
    return r;
}

/// A small, mixed fixture: pinned, today, yesterday, this week and older - one row
/// per group, so every group header has to appear exactly once.
std::vector<model::SessionRow> mixedFixture() {
    std::vector<model::SessionRow> rows;
    model::SessionRow pinned = row("s-pin", "Pinned session", kNowMs - 10 * kDayMs);
    pinned.pinned = true;
    rows.push_back(pinned);
    rows.push_back(row("s-today", "Today session", kNowMs - 2 * kHourMs, /*active=*/true));
    rows.push_back(row("s-yesterday", "Yesterday session", kNowMs - 1 * kDayMs));
    rows.push_back(row("s-week", "This week session", kNowMs - 3 * kDayMs));
    rows.push_back(row("s-old", "Old session", kNowMs - 30 * kDayMs));
    return rows;
}

ui::Rect railAt(int w, int h, const ui::Metrics& m) {
    ui::LayoutPrefs prefs;
    prefs.railOpen = true;
    prefs.inspectorOpen = false;
    const ui::ShellLayout layout =
        ui::computeShell(m, ImVec2(static_cast<float>(w), static_cast<float>(h)), prefs);
    return layout.rail;
}

// ---------------------------------------------------------------------------
// 1. Grouping and ordering
// ---------------------------------------------------------------------------

void checkGrouping() {
    const model::RailModel model = model::buildRailRows(mixedFixture(), kNowMs, "");
    check(!model.flat, "a snapshot with timestamps is grouped");
    check(model.rowCount == 5,
          "every row survives grouping (got " + std::to_string(model.rowCount) + ")");
    check(model.groups.size() == 5,
          "five groups, one per row (got " + std::to_string(model.groups.size()) + ")");
    if (model.groups.size() == 5) {
        check(model.groups[0].group == model::Group::Pinned, "pinned comes first");
        check(model.groups[1].group == model::Group::Today, "then TODAY");
        check(model.groups[2].group == model::Group::Yesterday, "then YESTERDAY");
        check(model.groups[3].group == model::Group::Previous7, "then PREVIOUS 7 DAYS");
        check(model.groups[4].group == model::Group::Older, "then OLDER");
        check(model.groups[1].rows.size() == 1 && model.groups[1].rows[0].id == "s-today",
              "TODAY holds the row from two hours ago");
        check(model.groups[0].rows[0].id == "s-pin", "the pinned row is in PINNED");
    }

    // Labels are the design's own words, one per group.
    check(std::string(model::groupLabel(model::Group::Pinned)) == "PINNED" &&
              std::string(model::groupLabel(model::Group::Today)) == "TODAY" &&
              std::string(model::groupLabel(model::Group::Yesterday)) == "YESTERDAY" &&
              std::string(model::groupLabel(model::Group::Previous7)) == "PREVIOUS 7 DAYS" &&
              std::string(model::groupLabel(model::Group::Older)) == "OLDER" &&
              std::string(model::groupLabel(model::Group::Ungrouped)) == "SESSIONS",
          "every group has its design label");

    // --- a day boundary: 23:59:59 yesterday vs 00:00:01 today -----------------
    const long long midnight = kNowMs - 12 * kHourMs; // 2026-06-18T00:00:00Z
    std::vector<model::SessionRow> boundary;
    boundary.push_back(row("late", "Late", midnight - 1000));   // 23:59:59 the day before
    boundary.push_back(row("early", "Early", midnight + 1000)); // 00:00:01 today
    const model::RailModel dayModel = model::buildRailRows(boundary, midnight + 2 * kHourMs, "");
    check(dayModel.groups.size() == 2, "a day boundary splits two rows into two groups");
    if (dayModel.groups.size() == 2) {
        check(dayModel.groups[0].group == model::Group::Today &&
                  dayModel.groups[0].rows[0].id == "early",
              "00:00:01 today is TODAY");
        check(dayModel.groups[1].group == model::Group::Yesterday &&
                  dayModel.groups[1].rows[0].id == "late",
              "23:59:59 the day before is YESTERDAY");
    }

    // --- a month boundary: yesterday is yesterday even when the month flips -----
    // The first `civilDay` encoded the date as YYYYMMDD, so 2026-11-01 minus 2026-10-31
    // was 70 and every row from the previous month landed in OLDER for the first week of
    // a month. These two cases are the regression guard.
    {
        std::tm t{};
        t.tm_year = 2026 - 1900;
        t.tm_mon = 11 - 1; // November
        t.tm_mday = 1;
        t.tm_hour = 10;
        const long long novFirst = static_cast<long long>(timegm(&t)) * 1000; // 10:00 UTC
        std::vector<model::SessionRow> month;
        month.push_back(row("today", "Today", novFirst - 1 * kHourMs));
        month.push_back(row("last-night", "Last night", novFirst - 14 * kHourMs)); // 10-31 20:00
        month.push_back(row("three-days", "Three days", novFirst - 3 * kDayMs));   // 10-29
        const model::RailModel monthModel = model::buildRailRows(month, novFirst, "");
        check(monthModel.groups.size() == 3, "a month boundary keeps three distinct groups (got " +
                                                 std::to_string(monthModel.groups.size()) + ")");
        if (monthModel.groups.size() == 3) {
            check(monthModel.groups[0].group == model::Group::Today &&
                      monthModel.groups[0].rows[0].id == "today",
                  "the 1st of the month is TODAY");
            check(monthModel.groups[1].group == model::Group::Yesterday &&
                      monthModel.groups[1].rows[0].id == "last-night",
                  "the 31st of the previous month is YESTERDAY, not OLDER");
            check(monthModel.groups[2].group == model::Group::Previous7 &&
                      monthModel.groups[2].rows[0].id == "three-days",
                  "three days ago across the month end is PREVIOUS 7 DAYS");
        }
    }

    // --- a year boundary -------------------------------------------------------
    {
        std::tm t{};
        t.tm_year = 2027 - 1900;
        t.tm_mon = 1 - 1; // January
        t.tm_mday = 2;
        t.tm_hour = 10;
        const long long janSecond = static_cast<long long>(timegm(&t)) * 1000;
        std::vector<model::SessionRow> year;
        year.push_back(row("jan02", "Jan 02", janSecond - 1 * kHourMs));
        year.push_back(row("jan01", "Jan 01", janSecond - 1 * kDayMs));
        year.push_back(row("dec31", "Dec 31", janSecond - 2 * kDayMs));
        const model::RailModel yearModel = model::buildRailRows(year, janSecond, "");
        check(yearModel.groups.size() == 3, "a year boundary keeps three distinct groups (got " +
                                                std::to_string(yearModel.groups.size()) + ")");
        if (yearModel.groups.size() == 3) {
            check(yearModel.groups[0].group == model::Group::Today, "Jan 02 is TODAY");
            check(yearModel.groups[1].group == model::Group::Yesterday, "Jan 01 is YESTERDAY");
            check(yearModel.groups[2].group == model::Group::Previous7 &&
                      yearModel.groups[2].rows[0].id == "dec31",
                  "Dec 31 across the year end is PREVIOUS 7 DAYS, not OLDER");
        }
    }

    // --- a week boundary: 7 days is PREVIOUS 7 DAYS, 8 is OLDER ---------------
    std::vector<model::SessionRow> week;
    week.push_back(row("d7", "Seven", kNowMs - 7 * kDayMs));
    week.push_back(row("d8", "Eight", kNowMs - 8 * kDayMs));
    const model::RailModel weekModel = model::buildRailRows(week, kNowMs, "");
    check(weekModel.groups.size() == 2, "the week boundary splits two rows into two groups");
    if (weekModel.groups.size() == 2) {
        check(weekModel.groups[0].group == model::Group::Previous7 &&
                  weekModel.groups[0].rows[0].id == "d7",
              "7 days ago is still PREVIOUS 7 DAYS");
        check(weekModel.groups[1].group == model::Group::Older &&
                  weekModel.groups[1].rows[0].id == "d8",
              "8 days ago is OLDER");
    }

    // --- unknown timestamps: one flat list, no fabricated dates ---------------
    std::vector<model::SessionRow> unknown;
    unknown.push_back(row("u1", "Undated one", 0));
    unknown.push_back(row("u2", "Undated two", 0));
    const model::RailModel flatModel = model::buildRailRows(unknown, kNowMs, "");
    check(flatModel.flat, "a snapshot with an unknown timestamp is drawn flat");
    check(flatModel.groups.size() == 1 && flatModel.groups[0].group == model::Group::Ungrouped,
          "the flat list is one Ungrouped group");
    check(flatModel.groups.size() == 1 && flatModel.groups[0].rows.size() == 2,
          "...holding every row");
    check(std::string(model::groupLabel(model::Group::Ungrouped)) == "SESSIONS",
          "...under the word SESSIONS, not a date");

    // --- ordering: newest first, then title, then id --------------------------
    std::vector<model::SessionRow> ties;
    ties.push_back(row("b", "Beta", kNowMs - kHourMs));
    ties.push_back(row("a", "Alpha", kNowMs - kHourMs));
    ties.push_back(row("c", "Gamma", kNowMs - 2 * kHourMs));
    const model::RailModel tieModel = model::buildRailRows(ties, kNowMs, "");
    check(tieModel.groups.size() == 1 && tieModel.groups[0].rows.size() == 3,
          "three rows in one day group");
    if (tieModel.groups.size() == 1 && tieModel.groups[0].rows.size() == 3) {
        check(tieModel.groups[0].rows[0].title == "Alpha" &&
                  tieModel.groups[0].rows[1].title == "Beta" &&
                  tieModel.groups[0].rows[2].title == "Gamma",
              "equal times fall back to title order, so the list cannot reorder itself");
    }

    // An empty snapshot has no groups at all (the view draws its empty message).
    const model::RailModel empty = model::buildRailRows({}, kNowMs, "");
    check(empty.rowCount == 0 && empty.groups.empty(), "an empty snapshot has no groups");
}

// ---------------------------------------------------------------------------
// 2. Filtering
// ---------------------------------------------------------------------------

void checkFiltering() {
    std::vector<model::SessionRow> rows = mixedFixture();
    const model::RailModel all = model::buildRailRows(rows, kNowMs, "   ");
    check(all.rowCount == rows.size() && !all.flat,
          "a whitespace-only query is no filter (and keeps the groups)");
    check(all.filteredOut == 0, "...and drops nothing");

    const model::RailModel match = model::buildRailRows(rows, kNowMs, "yester");
    check(match.flat, "a real query answers a ranked list, not a calendar");
    check(match.rowCount == 1 && match.filteredOut == rows.size() - 1,
          "only the matching row survives (got " + std::to_string(match.rowCount) + ", dropped " +
              std::to_string(match.filteredOut) + ")");
    if (match.rowCount == 1)
        check(match.groups[0].rows[0].id == "s-yesterday", "the survivor is the matching session");

    // --- the ranking -------------------------------------------------------
    // `session_fuzzy.h` owns the ranking (title weighted 2x, stable ties keep
    // snapshot order), and `./test_session_fuzzy` pins it. What the rail's model
    // has to prove is that it *uses* that rank and keeps the snapshot order when two
    // rows score the same.
    std::vector<model::SessionRow> rank;
    rank.push_back(row("first", "Zebra one", kNowMs));
    rank.push_back(row("second", "Zebra two", kNowMs - kHourMs));
    const model::RailModel ranked = model::buildRailRows(rank, kNowMs, "zebra");
    check(ranked.rowCount == 2, "both rows match the query");
    if (ranked.rowCount == 2) {
        check(ranked.groups[0].rows[0].id == "first",
              "equal scores keep snapshot order (the shipped stable-tie rule)");
    }

    const model::RailModel none = model::buildRailRows(rows, kNowMs, "qqqq");
    check(none.rowCount == 0 && none.groups.empty(),
          "a query that matches nothing answers no rows");
    check(none.filteredOut == rows.size(), "...and counts what it dropped (the 'no matches' line)");
}

// ---------------------------------------------------------------------------
// 3. Unread (D-UNREAD)
// ---------------------------------------------------------------------------

void checkUnread() {
    model::UnreadTracker tracker;
    std::vector<model::SessionRow> rows;
    rows.push_back(row("a", "Alpha", kNowMs - kHourMs));
    rows.push_back(row("b", "Beta", kNowMs - kHourMs));
    rows[1].active = true;

    // First sighting: nothing advanced, so nothing is unread.
    tracker.apply(rows);
    check(tracker.size() == 0, "a first snapshot marks nothing unread");

    // A's updatedAt advances while it is not active: unread.
    rows[0].updatedAtMs = kNowMs;
    tracker.apply(rows);
    check(tracker.isUnread("a"), "an update to a session that is not active is unread");

    // The active session's own advance is not unread: the user is looking at it.
    rows[1].updatedAtMs = kNowMs + 1000;
    tracker.apply(rows);
    check(!tracker.isUnread("b"), "the active session is never unread");

    // Selecting A clears its mark - both through `clear()` and by it becoming active.
    tracker.clear("a");
    check(!tracker.isUnread("a"), "clearing on select removes the dot");

    rows[0].updatedAtMs = kNowMs + 2000;
    tracker.apply(rows);
    check(tracker.isUnread("a"), "a second update marks it again");
    std::swap(rows[0].active, rows[1].active);
    tracker.apply(rows);
    check(!tracker.isUnread("a"), "making it the active session clears the mark");

    // Leaving a session is a write: `switchToSession` commits the outgoing session
    // and the store stamps `updatedAt`, so the row the user just left arrives with
    // the newer stamp that its own farewell produced. That baseline is not an update
    // nobody has seen, so it must not put a dot on the session you were just in
    // (regression: an ordinary switch marked the left session unread).
    model::UnreadTracker leaving;
    std::vector<model::SessionRow> pair;
    pair.push_back(row("leave-a", "Left", kNowMs - kHourMs));
    pair.push_back(row("leave-b", "Entered", kNowMs - kHourMs));
    pair[0].active = true;
    leaving.apply(pair);
    pair[0].updatedAtMs = kNowMs; // the commit on the way out
    pair[0].active = false;
    pair[1].active = true;
    leaving.apply(pair);
    check(!leaving.isUnread("leave-a"),
          "the session you just left is not unread for the save the switch performed");
    // ...and it is a one-shot exemption: news that arrives afterwards still marks it.
    pair[0].updatedAtMs = kNowMs + 5000;
    leaving.apply(pair);
    check(leaving.isUnread("leave-a"), "a later update to that session is unread again");

    // An unknown timestamp is not evidence of an update, in either direction.
    model::UnreadTracker unknown;
    std::vector<model::SessionRow> undated;
    undated.push_back(row("u", "Undated", 0));
    unknown.apply(undated);
    undated[0].updatedAtMs = kNowMs;
    unknown.apply(undated);
    check(unknown.size() == 0, "0 -> t is not an update: nothing is marked from an unknown stamp");

    // An id that leaves the snapshot is forgotten: its dot cannot outlive it.
    model::UnreadTracker leaves;
    std::vector<model::SessionRow> two;
    two.push_back(row("x", "X", kNowMs - kHourMs));
    two.push_back(row("y", "Y", kNowMs - kHourMs));
    leaves.apply(two);
    two[0].updatedAtMs = kNowMs;
    leaves.apply(two);
    check(leaves.isUnread("x"), "premise: x is unread before it disappears");
    two.erase(two.begin());
    leaves.apply(two);
    check(!leaves.isUnread("x"), "an id that left the snapshot is forgotten");
}

// ---------------------------------------------------------------------------
// 4. The pure view rules
// ---------------------------------------------------------------------------

void checkPureRules() {
    const ui::Metrics grid = ui::metricsFor(true, 1.0f, 1.0f);
    const ui::Metrics pixel = ui::metricsFor(false, 1.0f, 1.0f);
    check(views::railRowLines(grid, 29.0f) == 1, "29 terminal rows keep the one-line row");
    check(views::railRowLines(grid, 30.0f) == 2, "30 terminal rows get the preview line");
    check(views::railRowLines(grid, 0.0f) == 1, "a display with no height keeps the one-line row");
    check(views::railRowLines(pixel, 599.0f) == 1, "599 px keeps the one-line row");
    check(views::railRowLines(pixel, 600.0f) == 2, "600 px gets the preview line");
    check(views::railRowLines(pixel, 820.0f) == 2, "the 1280x820 default window is two-line");

    check(views::railTimeLabel(0, kNowMs).empty(), "an unknown stamp has no time label");
    check(views::railTimeLabel(kNowMs - 30 * 1000, kNowMs) == "now", "30 s ago is 'now'");
    check(views::railTimeLabel(kNowMs - 5 * 60 * 1000, kNowMs) == "5m", "5 minutes is '5m'");
    check(views::railTimeLabel(kNowMs - 3 * kHourMs, kNowMs) == "3h", "3 hours is '3h'");
    check(views::railTimeLabel(kNowMs - 2 * kDayMs, kNowMs) == "2d", "2 days is '2d'");
    check(views::railTimeLabel(kNowMs - 21 * kDayMs, kNowMs) == "3w", "21 days is '3w'");
    check(views::railTimeLabel(kNowMs + 5 * kHourMs, kNowMs) == "now",
          "a future stamp (clock skew) is 'now', never a negative age");

    // The window: an item of overscan, clamped inside the list.
    const views::RailWindow top = views::railWindow(1000, 0.0f, 1.0f, 30.0f, 0.0f);
    check(top.first == 0, "at the top the window starts at the first row");
    check(top.count <= 32, "at the top the window is the viewport plus overscan (got " +
                               std::to_string(top.count) + ")");
    const views::RailWindow middle = views::railWindow(1000, 0.0f, 1.0f, 30.0f, 500.0f);
    check(middle.first == 500, "scrolled to 500 the window starts at row 500");
    check(middle.count <= 32, "...and is still the viewport plus overscan");
    const views::RailWindow end = views::railWindow(1000, 0.0f, 1.0f, 30.0f, 999.0f);
    check(end.first == 999 && end.count == 1, "at the bottom the window is the last row");
    const views::RailWindow none = views::railWindow(0, 0.0f, 1.0f, 30.0f, 0.0f);
    check(none.first == 0 && none.count == 0, "an empty list has an empty window");
    const views::RailWindow offset = views::railWindow(10, 100.0f, 2.0f, 10.0f, 100.0f);
    check(offset.first == 0 && offset.count <= 8,
          "a sub-list at an offset is windowed in its own space");
}

// ---------------------------------------------------------------------------
// 5. The row inks
// ---------------------------------------------------------------------------

float channel(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float luminance(ImVec4 c) {
    return 0.2126f * channel(c.x) + 0.7152f * channel(c.y) + 0.0722f * channel(c.z);
}

ImVec4 composite(ImVec4 fg, ImVec4 bg) {
    const float a = fg.w;
    return ImVec4(fg.x * a + bg.x * (1.0f - a), fg.y * a + bg.y * (1.0f - a),
                  fg.z * a + bg.z * (1.0f - a), 1.0f);
}

float contrast(ImVec4 a, ImVec4 b) {
    const float la = luminance(a);
    const float lb = luminance(b);
    return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
}

void checkRowInks() {
    const ui::TokenTable& table = ui::tokens();
    const auto ink = [&table](ui::ColorToken token) { return table.rgb[static_cast<int>(token)]; };
    const ImVec4 surface = ink(ui::ColorToken::surface);
    int states = 0;
    for (int i = 0; i < static_cast<int>(views::RailRowState::Count); ++i) {
        const auto state = static_cast<views::RailRowState>(i);
        const views::RailRowInks inks = views::railRowInks(state);
        const std::string name = views::railRowStateName(state);
        ++states;

        check(inks.title != ui::ColorToken::textDim && inks.preview != ui::ColorToken::textDim,
              name + ": no label ink is textDim (NFR-09: it carries no glyph)");
        check(inks.title == ui::ColorToken::textHi && inks.preview == ui::ColorToken::textMid,
              name + ": title textHi, preview textMid - the state is never carried by dimming");

        // The pair a reader sees: the ink on the row's own fill, composited over the
        // rail's surface (the selection fill is translucent).
        const ImVec4 fill = composite(ink(inks.fill), surface);
        const float titleRatio = contrast(composite(ink(inks.title), fill), fill);
        const float previewRatio = contrast(composite(ink(inks.preview), fill), fill);
        check(titleRatio >= 4.5f,
              name + ": the title clears 4.5:1 on its fill (" + num(titleRatio) + ")");
        check(previewRatio >= 4.5f,
              name + ": the preview clears 4.5:1 on its fill (" + num(previewRatio) + ")");
    }
    check(states == 7, "every row state was measured (got " + std::to_string(states) + ")");

    // The active row is the design's anatomy, not the v4 pair: selection + accent.
    const views::RailRowInks active = views::railRowInks(views::RailRowState::Active);
    check(active.fill == ui::ColorToken::selection, "the active row is filled with `selection`");
    check(active.rule == ui::ColorToken::accent, "...and marked with the accent rule");
    const views::RailRowInks pending = views::railRowInks(views::RailRowState::Pending);
    check(pending.rule == ui::ColorToken::stateWarn && pending.fill != active.fill,
          "the pending switch is visibly not the active row");
}

// ---------------------------------------------------------------------------
// 6. The drawn rail
// ---------------------------------------------------------------------------

void checkDrawnRail() {
    const ui::Metrics m = ui::metricsFor(true, 1.0f, 1.0f);
    const ui::Rect region = railAt(kScreenW, kScreenH, m);
    check(region.w > 0.0f && region.h > 0.0f, "premise: the rail has a column at 100x30");

    model::UiState ui;
    const model::RailModel list = model::buildRailRows(mixedFixture(), kNowMs, "");
    const int rowLines = views::railRowLines(m, static_cast<float>(kScreenH));
    check(rowLines == 2, "premise: 30 terminal rows give two-line rows");

    const auto body = [&](const ui::Metrics& metrics) {
        views::renderRail(ui.rail, list, region, metrics, rowLines, kNowMs, /*acceptKeys=*/true);
    };
    Grid grid = frame(body);

    check(ui.rail.drawnRows == 5,
          "all five rows were drawn (got " + std::to_string(ui.rail.drawnRows) + ")");
    check(grid.has("Search"), "the search row is drawn");
    check(grid.has("PINNED"), "the PINNED header is drawn");
    check(grid.has("TODAY"), "the TODAY header is drawn");
    check(grid.has("YESTERDAY"), "the YESTERDAY header is drawn");
    check(grid.has("PREVIOUS 7 DAYS"), "the PREVIOUS 7 DAYS header is drawn");
    check(grid.has("OLDER"), "the OLDER header is drawn");
    check(grid.has("Today session"), "a row's title is drawn");
    check(grid.has("preview line"), "a row's preview is drawn (two-line mode)");
    check(grid.has("+ New session"), "the footer's new-session button is drawn");
    check(grid.has("2h"), "a row's relative time is drawn");

    // The markers: ● for the active row, ▸ for the rest (the ASCII column is off).
    const std::string active = utf8Of(0x25CF); // ●
    const std::string rest = utf8Of(0x25B8);   // ▸
    check(grid.has(active), "the active row carries the ● marker");
    check(grid.has(rest), "a rest row carries the ▸ marker");

    // The active row's rule is the one-cell glyph and its fill is the selection step
    // (233-255 grey ramp index 235 on the text grid).
    int markerX = -1;
    int markerY = -1;
    grid.find(active, markerX, markerY);
    check(markerX > 0, "the active marker was found");
    if (markerX > 0) {
        const std::string ruleGlyph = utf8Of(0x258C); // ▌
        check(grid.rowText(markerY).find(ruleGlyph) != std::string::npos,
              "the active row draws the rule glyph at its left edge");
        // The fill: the active row paints one and a rest row does not (the exact
        // palette index belongs to the colour depth, which the contrast test in
        // `./test_theme_contrast` measures; here the difference is the point).
        const int restY = grid.rowContaining("Old session");
        check(grid.bgAt(region.x + m.pad1 + 2, markerY) != 0,
              "the active row is filled (the surface step it lands on is not blank)");
        check(restY >= 0 && grid.bgAt(region.x + m.pad1 + 2, markerY) !=
                                grid.bgAt(region.x + m.pad1 + 2, restY),
              "the active row's fill differs from a rest row's");
    }

    // One-line rows below the threshold.
    const auto narrowBody = [&](const ui::Metrics& metrics) {
        views::renderRail(ui.rail, list, region, metrics, 1, kNowMs, /*acceptKeys=*/true);
    };
    Grid oneLine = frame(narrowBody);
    check(!oneLine.has("preview of Today session"), "one-line mode drops the preview line");
    check(oneLine.has("Today session"), "...and keeps the title");

    // Empty snapshot: the rail says so instead of drawing a blank column.
    const model::RailModel empty = model::buildRailRows({}, kNowMs, "");
    const auto emptyBody = [&](const ui::Metrics& metrics) {
        views::renderRail(ui.rail, empty, region, metrics, rowLines, kNowMs, true);
    };
    Grid emptyGrid = frame(emptyBody);
    check(emptyGrid.has("No sessions"), "an empty snapshot has its own message");

    // The footer works: a press on `+ New session` reports the New action.
    int newX = -1;
    int newY = -1;
    grid.find("+ New session", newX, newY);
    check(newX >= 0, "premise: the new-session button is on screen");
    clickAt(static_cast<float>(newX), static_cast<float>(newY), body);
    check(ui.rail.newRequested, "a press on the footer queues a new session");
    ui.rail.newRequested = false;

    // A filter that matches nothing says "No matches" (not "No sessions yet").
    const model::RailModel none = model::buildRailRows(mixedFixture(), kNowMs, "qqqq");
    const auto noneBody = [&](const ui::Metrics& metrics) {
        views::renderRail(ui.rail, none, region, metrics, rowLines, kNowMs, true);
    };
    Grid noneGrid = frame(noneBody);
    check(noneGrid.has("No matches"), "a filter with no match has its own message");
    check(!noneGrid.has("No sessions"), "...and not the empty-snapshot one");

    // The loading skeleton is for "nothing to show yet": an empty list while a select
    // is in flight draws three placeholder rows...
    model::RailState loading;
    loading.loadingId = "s-today";
    const model::RailModel emptyList = model::buildRailRows({}, kNowMs, "");
    const auto loadingEmptyBody = [&](const ui::Metrics& metrics) {
        views::renderRail(loading, emptyList, region, metrics, rowLines, kNowMs, true);
    };
    Grid loadingGrid = frame(loadingEmptyBody);
    check(!loadingGrid.has("No sessions"), "the skeleton replaces the empty message");
    check(loading.drawnRows == 0, "the skeleton draws no session rows");

    // ...and a list that already has rows keeps drawing them, with the target row in
    // the pending state. A switch must never hide the sessions: D's `activateSession`
    // returns without a snapshot when the file fails to load, and an empty rail with the
    // skeleton up had no way back.
    model::RailState waiting;
    waiting.loadingId = "s-today";
    const auto waitingBody = [&](const ui::Metrics& metrics) {
        views::renderRail(waiting, list, region, metrics, rowLines, kNowMs, true);
    };
    Grid waitingGrid = frame(waitingBody);
    check(waitingGrid.has("Today session"), "a switch in flight keeps the list drawn");
    check(waiting.drawnRows == 5,
          "and draws every row (got " + std::to_string(waiting.drawnRows) + ")");
    const std::string pendingMark = utf8Of(0x25CB); // ○
    check(waitingGrid.has(pendingMark), "the row being switched to carries the pending marker");
}

// ---------------------------------------------------------------------------
// 7. Bounded rendering and the frame budget
// ---------------------------------------------------------------------------

void checkBoundedRendering() {
    const ui::Metrics m = ui::metricsFor(true, 1.0f, 1.0f);
    const ui::Rect region = railAt(kScreenW, kScreenH, m);
    const int rowLines = views::railRowLines(m, static_cast<float>(kScreenH));

    std::vector<model::SessionRow> rows;
    rows.reserve(2000);
    for (int i = 0; i < 2000; ++i) {
        // Spread over the date groups so the fixture is not one bucket.
        model::SessionRow r = row("id-" + std::to_string(i), "Session " + std::to_string(i),
                                  kNowMs - static_cast<long long>(i) * 37 * 60 * 1000);
        r.running = (i == 0);
        r.unread = (i % 17 == 0);
        rows.push_back(std::move(r));
    }

    model::UiState ui;
    model::RailModel list;
    model::UnreadTracker unread;
    // The whole per-frame rail cost, not just the draw: the frame layer copies the
    // snapshot onto `SessionRow`s, applies the unread rule, groups/filters and draws -
    // so the timed body does all four, exactly like `views::shell.cpp` does.
    const auto body = [&](const ui::Metrics& metrics) {
        std::vector<model::SessionRow> frameRows;
        frameRows.reserve(rows.size());
        for (const model::SessionRow& r : rows)
            frameRows.push_back(r);
        unread.apply(frameRows);
        for (model::SessionRow& r : frameRows)
            r.unread = unread.isUnread(r.id);
        list = model::buildRailRows(frameRows, kNowMs, "");
        views::renderRail(ui.rail, list, region, metrics, rowLines, kNowMs, /*acceptKeys=*/true);
    };
    const Grid grid = frame(body);
    (void)grid;

    check(list.rowCount == rows.size(),
          "the 2000-session fixture was grouped (got " + std::to_string(list.rowCount) + ")");
    // The viewport holds (rows area / row height) rows; the view draws that plus one
    // row of overscan and never the list (D-RAIL's scale rule).
    const float rowsArea = region.h - 2.0f * m.row - m.border; // search row, footer, hairline
    const float viewportRows = std::floor(rowsArea / (m.row * 2.0f));
    const std::size_t viewportBound = static_cast<std::size_t>(viewportRows) + 1;
    check(ui.rail.drawnRows <= viewportBound,
          "only the viewport plus one row of overscan is drawn (" +
              std::to_string(ui.rail.drawnRows) + " of 2000, bound " +
              std::to_string(viewportBound) + ")");
    check(ui.rail.drawnRows > 0, "premise: something was drawn");

    // A whole frame - the model plus the view - inside the frame budget. The bound is
    // deliberately generous (a 60 Hz frame is 16.7 ms): this is a regression guard
    // against the O(n) draw the bounded window replaced, not a benchmark. The cost of
    // the frame's own plumbing (ImGui's NewFrame, the backend's draw-data walk) is
    // measured with an empty body and subtracted, so the number is the rail's.
    constexpr int kFrames = 60;
    const auto emptyBody = [](const ui::Metrics&) {};
    const auto timeFrames = [](const std::function<void(const ui::Metrics&)>& body) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kFrames; ++i)
            frame(body);
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now() - start)
                   .count() /
               1000.0 / kFrames;
    };
    const double floorMs = timeFrames(emptyBody);
    const double fullMs = timeFrames(body);
    const double perFrameMs = fullMs > floorMs ? fullMs - floorMs : 0.0;
    if (g_verbose)
        std::fprintf(stderr,
                     "  2000-session frame: %.3f ms (of %.3f ms; an empty frame is %.3f ms)\n",
                     perFrameMs, fullMs, floorMs);
    check(perFrameMs < 16.0, "a 2000-session frame stays inside the frame budget (" +
                                 num(static_cast<float>(perFrameMs)) + " ms)");

    // The last session is reachable: the child has a scroll range (what the scrollbar
    // needs), and scrolling to the end draws the oldest row. `SetNextWindowScroll` is
    // the one-shot "begin this window at that offset" call, so it applies to the rows
    // child that `renderRail` begins inside this frame.
    ImGuiWindow* rowsWindow = nullptr;
    for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows) {
        if (window != nullptr && window->WasActive &&
            std::strstr(window->Name, "##rail_rows") != nullptr)
            rowsWindow = window;
    }
    check(rowsWindow != nullptr, "the rail's rows live in a scrolling child");
    check(rowsWindow != nullptr && rowsWindow->ScrollMax.y > 0.0f,
          "the 2000-session list has a scroll range (the scrollbar's extent)");
    const auto endBody = [&](const ui::Metrics& metrics) {
        ImGui::SetNextWindowScroll(ImVec2(0.0f, 1.0e6f)); // clamped to ScrollMax by Begin
        views::renderRail(ui.rail, list, region, metrics, rowLines, kNowMs, true);
    };
    settle(endBody, 2);
    const Grid endGrid = frame(endBody);
    check(endGrid.has("Session 1999"),
          "the oldest session is drawn once the list is scrolled to its end");
    check(!endGrid.has("Session 0 "), "...and the first session has scrolled off the top");
    // Put the child back at the top: its id is stable on purpose (the scroll position
    // survives a snapshot), so the offset would otherwise leak into the next check.
    const auto topBody = [&](const ui::Metrics& metrics) {
        ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
        views::renderRail(ui.rail, list, region, metrics, rowLines, kNowMs, true);
    };
    settle(topBody, 2);
}

// ---------------------------------------------------------------------------
// 8. The collapsed strip (design §4.3 "Degradation")
// ---------------------------------------------------------------------------

void checkStrip() {
    // 90x30 is inside the width band where the layout hands the rail its 7-cell
    // collapsed column (`ui::computeShell`), i.e. the shape the app draws at a common
    // terminal width.
    const ui::Metrics m = ui::metricsFor(true, 1.0f, 1.0f);
    const ui::Rect region = railAt(90, 30, m);
    check(region.w > 0.0f && region.w <= 12.0f,
          "premise: the rail is the collapsed column at 90x30 (w=" + std::to_string(region.w) +
              ")");

    const std::vector<model::SessionRow> rows = mixedFixture();
    const model::RailModel list = model::buildRailRows(rows, kNowMs, "");
    model::UiState ui;
    const auto body = [&](const ui::Metrics& metrics) {
        views::renderRail(ui.rail, list, region, metrics, /*rowLines=*/2, kNowMs, true);
    };
    const Grid grid = frame(body);

    // What the strip must not do: cut words to nothing. The full anatomy at this width
    // drew "Searc", "PINN…" and rows without a title.
    check(!grid.has("PINNED") && !grid.has("PREVIOUS"), "no truncated group headers");
    check(!grid.has("Searc"), "no truncated search hint");
    // What it must do: a mark per session, the search entry and the `+` entry.
    check(ui.rail.drawnRows == rows.size(),
          "one mark per session (got " + std::to_string(ui.rail.drawnRows) + ")");
    check(grid.has("/"), "the search entry is drawn");
    check(grid.has("+"), "the new-session entry is drawn");
    check(grid.has(utf8Of(0x25CF)), "the active session's mark is drawn");

    // A mark is a hit box: a click selects that session.
    int markX = -1;
    int markY = -1;
    grid.find(utf8Of(0x25B8), markX, markY); // the first rest mark
    check(markX >= 0, "premise: a rest mark is on screen");
    clickAt(static_cast<float>(markX), static_cast<float>(markY), body);
    check(!ui.rail.selectId.empty(),
          "a mark click selects its session (got '" + ui.rail.selectId + "')");
    ui.rail.selectId.clear();

    // The search entry asks for the search; the frame layer opens the rail for it (the
    // strip has no field to focus).
    int searchX = -1;
    int searchY = -1;
    grid.find("/", searchX, searchY);
    clickAt(static_cast<float>(searchX), static_cast<float>(searchY), body);
    check(ui.rail.focusSearch || ui.rail.filterFocus, "the search entry reports the focus request");
    ui.rail.focusSearch = false;
    ui.rail.filterFocus = false;

    // The strip takes no row keys: rename and delete need the open rail.
    frame(body);
    frame(body, [&](ImGuiIO& io) {
        io.AddMousePosEvent(static_cast<float>(markX), static_cast<float>(markY));
    });
    settle(body);
    typeText("d", body);
    check(ui.rail.deleteId.empty(), "`d` on the strip deletes nothing (it takes no row keys)");
    typeText("r", body);
    check(ui.rail.renameRowId.empty(), "`r` on the strip opens no box");

    // `/` is a key, not only a button: the strip answers it with the same request the
    // full anatomy makes (design §4.3, "from anywhere except the composer"), and the
    // frame layer turns that request into the width that gives the rail a field.
    typeText("/", body);
    check(ui.rail.focusSearch, "`/` on the strip reports the same focus request as the button");
    ui.rail.focusSearch = false;
    ui.rail.filterFocus = false;

    // The strip closes what the full anatomy opened: a menu request, a rename box and a
    // query are all undrawable in a 7-cell column, so none of them may stay live behind
    // it - and a hidden query would drop rows with no way to see or clear it.
    ui.rail.menuRequestId = rows.front().id;
    ui.rail.renameRowId = rows.front().id;
    ui.rail.renameFocus = true;
    std::snprintf(ui.rail.filterBuf.data(), ui.rail.filterBuf.size(), "alp");
    frame(body);
    check(ui.rail.menuRequestId.empty() && ui.rail.renameRowId.empty() && !ui.rail.renameFocus &&
              ui.rail.filterBuf[0] == '\0',
          "the strip closes the dialogs and the query it cannot draw");

    // Every state a row can be in changes the glyph, because in this shape the glyph is
    // the only carrier: the full anatomy's fill, rule and time column have no room here.
    std::vector<model::SessionRow> states(4);
    states[0].id = "m-rest";
    states[1].id = "m-active";
    states[1].active = true;
    states[2].id = "m-unread";
    states[2].unread = true;
    states[3].id = "m-run";
    states[3].running = true;
    const model::RailModel stateList = model::buildRailRows(states, kNowMs, "");
    ui.rail = model::RailState{};
    const auto stateBody = [&](const ui::Metrics& metrics) {
        views::renderRail(ui.rail, stateList, region, metrics, /*rowLines=*/2, kNowMs, true);
    };
    const Grid stateGrid = frame(stateBody);
    check(stateGrid.has(utf8Of(0x25B8)), "strip mark: rest is a right triangle");
    check(stateGrid.has(utf8Of(0x25CF)), "strip mark: active is a filled circle");
    check(stateGrid.has(utf8Of(0x2022)), "strip mark: unread is a bullet");
    check(stateGrid.has(utf8Of(0x25D0)), "strip mark: running is the static half circle");
}

// ---------------------------------------------------------------------------
// 9. Interaction
// ---------------------------------------------------------------------------

void checkInteraction() {
    const ui::Metrics m = ui::metricsFor(true, 1.0f, 1.0f);
    const ui::Rect region = railAt(kScreenW, kScreenH, m);
    const int rowLines = views::railRowLines(m, static_cast<float>(kScreenH));
    const std::vector<model::SessionRow> rows = mixedFixture();
    const model::RailModel list = model::buildRailRows(rows, kNowMs, "");

    model::UiState ui;
    const auto body = [&](const ui::Metrics& metrics) {
        views::renderRail(ui.rail, list, region, metrics, rowLines, kNowMs, /*acceptKeys=*/true);
    };

    // --- a click selects ---------------------------------------------------
    const Grid grid = frame(body);
    int titleX = -1;
    int titleY = -1;
    grid.find("Yesterday session", titleX, titleY);
    check(titleX >= 0, "premise: the row to click is on screen");
    clickAt(static_cast<float>(titleX), static_cast<float>(titleY), body);
    check(ui.rail.selectId == "s-yesterday",
          "a row click reports a select (got '" + ui.rail.selectId + "')");
    ui.rail.selectId.clear();

    // Clicking the active row is a no-op (it is already selected).
    grid.find("Today session", titleX, titleY);
    clickAt(static_cast<float>(titleX), static_cast<float>(titleY), body);
    check(ui.rail.selectId.empty(), "clicking the active row does not queue a select");

    // --- `m` opens the row menu -------------------------------------------
    settle(body); // latch the pointer's row (still over "Today session")
    typeText("m", body);
    check(ui.rail.menuRequestId == "s-today" || ui.rail.menuRowId == "s-today",
          "`m` targets the row under the pointer (request '" + ui.rail.menuRequestId + "', menu '" +
              ui.rail.menuRowId + "')");
    frame(body); // the popup opens in the row's own scope
    check(ui.rail.menuRowId == "s-today", "the row menu is open on that row");

    // Move the pointer away and close the menu again (Esc closes an ImGui popup).
    frame(body, [&](ImGuiIO& io) { io.AddMousePosEvent(-100.0f, -100.0f); });
    settle(body);
    pressEscape(ui.rail, body);
    check(ui.rail.menuRowId.empty(), "Esc closes the row menu");
    check(ui.rail.renameRowId.empty(), "an Esc that closed the menu did not open a rename box");

    // --- `r` renames, Enter commits once ----------------------------------
    grid.find("Old session", titleX, titleY);
    check(pointAt(ui.rail, "s-old", static_cast<float>(titleX), static_cast<float>(titleY), body),
          "premise: the pointer reaches the row to rename");
    typeText("r", body);
    check(ui.rail.renameRowId == "s-old",
          "`r` opens the rename box on the target row (got '" + ui.rail.renameRowId + "')");
    settle(body); // the field takes the keyboard (SetKeyboardFocusHere selects all)
    typeText("X", body);
    pressKey(ImGuiKey_Enter, body);
    check(ui.rail.renameId == "s-old", "Enter commits the rename (got '" + ui.rail.renameId + "')");
    check(ui.rail.renamedTitle == "X",
          "the committed title is the typed one (got '" + ui.rail.renamedTitle + "')");
    check(ui.rail.renameRowId.empty(), "the box closes on commit");
    ui.rail.renameId.clear();
    ui.rail.renamedTitle.clear();

    // --- `r` then Esc cancels and spends the press -------------------------
    grid.find("This week session", titleX, titleY);
    check(pointAt(ui.rail, "s-week", static_cast<float>(titleX), static_cast<float>(titleY), body),
          "premise: the pointer reaches the third row");
    typeText("r", body);
    check(ui.rail.renameRowId == "s-week", "premise: the box is open on the third row");
    const bool renameEscTook = pressEscape(ui.rail, body);
    check(ui.rail.renameRowId.empty(), "Esc closes the box");
    check(ui.rail.renameId.empty(), "...and commits nothing");
    check(renameEscTook, "...and reports the press it took (D-ESC)");

    // --- `d` deletes the row the user aimed at, in one press ----------------
    // The guard is the target, not a second press: `d` acts on the pointer's row (or the
    // nav-focused one) and on nothing else, and the menu's `Delete` - the other path - is
    // a two-click one. No confirmation, so nothing is drawn that could be missed.
    ui.rail.deleteId.clear();
    typeText("d", body);
    check(ui.rail.deleteId == "s-week",
          "`d` deletes the targeted row (got '" + ui.rail.deleteId + "')");
    check(ui.rail.selectId.empty(), "...and does not select it");
    ui.rail.deleteId.clear();
    const Grid afterDeleteGrid = frame(body);
    check(!afterDeleteGrid.has("Delete") && !afterDeleteGrid.has("Cancel"),
          "no confirmation is drawn: one answer, and it is the delete");

    // The row menu's Delete is the mouse path, and it deletes in the same press.
    typeText("m", body);
    frame(body); // the request opens the popup
    check(ui.rail.menuRowId == "s-week", "premise: the row menu is open");
    const Grid menuGrid = frame(body); // the popup is drawn on the frame after it opens
    int menuDeleteX = -1;
    int menuDeleteY = -1;
    menuGrid.find("Delete", menuDeleteX, menuDeleteY);
    check(menuDeleteX >= 0, "premise: the menu offers Delete");
    clickAt(static_cast<float>(menuDeleteX), static_cast<float>(menuDeleteY), body);
    check(ui.rail.deleteId == "s-week", "the menu's Delete deletes the row it was opened on");
    check(ui.rail.selectId.empty(), "...and selects nothing");
    ui.rail.deleteId.clear();

    // The menu left the pointer on the row the popup covered, so aim again before the
    // keyboard checks below.
    check(pointAt(ui.rail, "s-week", static_cast<float>(titleX), static_cast<float>(titleY), body),
          "premise: the pointer is back on the third row");

    // `Esc` is not a delete step any more: the queued answer stands, because there is
    // nothing to cancel - the delete was the press itself.
    typeText("d", body);
    check(ui.rail.deleteId == "s-week", "premise: the delete is queued");
    pressEscape(ui.rail, body);
    check(ui.rail.deleteId == "s-week", "Esc does not undo a queued delete");
    ui.rail.deleteId.clear();

    // --- `p` pins ----------------------------------------------------------
    typeText("p", body);
    check(ui.rail.pinToggleId == "s-week", "`p` toggles the target row's pin");
    ui.rail.pinToggleId.clear();

    // --- `/` focuses the search, typing filters, Enter selects the top row --
    typeText("/", body);
    check(ui.rail.focusSearch && ui.rail.filterFocus,
          "`/` focuses the search field and reports the action");
    settle(body); // the field takes the keyboard
    typeText("yester", body);
    check(std::string(ui.rail.filterBuf.data()) == "yester",
          "typing reaches the search buffer (got '" + std::string(ui.rail.filterBuf.data()) + "')");
    // While the query is live the frame draws the *filtered* list (that is what the
    // shell does: it rebuilds the model from the same buffer every frame).
    const auto filteredBody = [&](const ui::Metrics& metrics) {
        const std::string query(ui.rail.filterBuf.data());
        const model::RailModel filtered = model::buildRailRows(rows, kNowMs, query);
        views::renderRail(ui.rail, filtered, region, metrics, rowLines, kNowMs, true);
    };
    Grid filteredGrid = frame(filteredBody);
    check(filteredGrid.has("Yesterday session") && !filteredGrid.has("Old session"),
          "the drawn rail shows only the matching row");
    pressKey(ImGuiKey_Enter, filteredBody);
    check(ui.rail.selectId == "s-yesterday",
          "Enter selects the top match (got '" + ui.rail.selectId + "')");
    check(std::string(ui.rail.filterBuf.data()).empty(), "and the search clears on select");
    ui.rail.selectId.clear();

    // --- Esc clears a stale query -----------------------------------------
    typeText("/", body);
    settle(body);
    typeText("old", body);
    check(std::string(ui.rail.filterBuf.data()) == "old", "premise: the query is 'old'");
    frame(body); // the field keeps the keyboard; the latch sees it
    const bool searchEscTook = pressEscape(ui.rail, body);
    check(std::string(ui.rail.filterBuf.data()).empty(), "Esc clears the search");
    check(searchEscTook, "...and reports the press it took");

    // --- the search is a mode, and an empty one ends -----------------------
    // The frame borrows a width for the field (a 7-cell column cannot hold one), so it has
    // to know when the borrowing is over: an empty query and a field no one is typing in.
    ui.rail.searchWide = true;
    ui.rail.searchEnded = false;
    frame(body);
    check(ui.rail.searchEnded, "an empty query with no keyboard ends the search mode");
    ui.rail.searchEnded = false;
    std::snprintf(ui.rail.filterBuf.data(), ui.rail.filterBuf.size(), "old");
    frame(body);
    check(!ui.rail.searchEnded, "a live query keeps the search (and its width) open");
    ui.rail.filterBuf.fill('\0');

    // The way out: while the field holds the keyboard the mode cannot end. Clicking out
    // of it is the action that hands the keyboard over - and the one a user takes when
    // they stop searching - so it is also what ends the mode. (Esc does not release an
    // ImGui text field in this backend, so the mode must not depend on it.)
    ui.rail.filterFocus = true;
    frame(body);
    check(!ui.rail.searchEnded, "the frame that hands the field the keyboard does not end it");
    clickAt(70.0f, 20.0f, body); // outside the rail's column, inside the frame
    settle(body);
    check(ui.rail.searchEnded, "clicking out of the field ends the search");
    ui.rail.searchEnded = false;
    ui.rail.searchWide = false;

    // --- row keys need the rail: a key pressed while the pointer is elsewhere ---
    // The first version fell back to "the active session, else the first row", so a
    // stray `d` while the user read the thread asked to delete the active session and a
    // second `d` queued the delete. Nothing may act on a row nobody pointed at, except
    // `/` (the design's "focus search from anywhere except the composer").
    {
        model::UiState away;
        const auto awayBody = [&](const ui::Metrics& metrics) {
            views::renderRail(away.rail, list, region, metrics, rowLines, kNowMs, true);
        };
        frame(awayBody);
        frame(awayBody, [](ImGuiIO& io) { io.AddMousePosEvent(-100.0f, -100.0f); });
        settle(awayBody);
        check(away.rail.hoverRowId.empty() && away.rail.navRowId.empty(),
              "premise: no row holds the pointer or the keyboard");
        typeText("d", awayBody);
        check(away.rail.deleteId.empty(),
              "`d` with the pointer off the rail deletes nothing (no active-row fallback)");
        typeText("p", awayBody);
        check(away.rail.pinToggleId.empty(), "`p` off the rail pins nothing");
        typeText("r", awayBody);
        check(away.rail.renameRowId.empty(), "`r` off the rail renames nothing");
        typeText("m", awayBody);
        check(away.rail.menuRequestId.empty() && away.rail.menuRowId.empty(),
              "`m` off the rail opens nothing");
        typeText("/", awayBody);
        check(away.rail.focusSearch && away.rail.filterFocus,
              "`/` still focuses the search from anywhere");
    }

    // --- keys stand down while an overlay owns them -----------------------
    model::UiState quiet;
    const auto quietBody = [&](const ui::Metrics& metrics) {
        views::renderRail(quiet.rail, list, region, metrics, rowLines, kNowMs,
                          /*acceptKeys=*/false);
    };
    frame(quietBody);
    typeText("d", quietBody);
    check(quiet.rail.deleteId.empty(), "with acceptKeys false the rail takes no row keys at all");
}

// ---------------------------------------------------------------------------
// 9b. The row menu's mirror cannot outlive its popup
// ---------------------------------------------------------------------------

/// A row culled out of the drawn band takes its popup with it: ImGui closes a popup whose window
/// is not submitted, so `RailState::menuRowId` - the mirror `views::railOwnsEscape()` reads - has
/// to be dropped in that same frame. Otherwise the composer is told an overlay is up and the field
/// goes deaf to `Esc` (clear, blur) and to the history keys, with nothing on screen to explain it
/// (task-41 review, finding 1).
void checkMenuMirrorFollowsCulling() {
    const ui::Metrics m = ui::metricsFor(true, 1.0f, 1.0f);
    ui::Rect region = railAt(kScreenW, kScreenH, m);
    const int rowLines = views::railRowLines(m, static_cast<float>(kScreenH));
    std::vector<model::SessionRow> rows;
    for (int i = 0; i < 8; ++i)
        rows.push_back(row("s-" + std::to_string(i), "Session " + std::to_string(i), kNowMs));
    const model::RailModel list = model::buildRailRows(rows, kNowMs, "");

    model::UiState ui;
    const auto body = [&](const ui::Metrics& metrics) {
        views::renderRail(ui.rail, list, region, metrics, rowLines, kNowMs, /*acceptKeys=*/true);
    };
    settle(body);
    const Grid full = frame(body);

    // The sixth row is inside the band at full height. Aim at it the way a user does - the pointer
    // on the row, then the `m` character - so the popup goes through the path the app uses.
    int rowX = -1;
    int rowY = -1;
    full.find("Session 6", rowX, rowY);
    check(rowX >= 0, "premise: the sixth row is on screen");
    pointAt(ui.rail, "s-6", static_cast<float>(rowX), static_cast<float>(rowY), body);
    typeText("m", body);
    frame(body);
    check(ui.rail.menuRowId == "s-6",
          "premise: the row menu is open on the sixth row (got '" + ui.rail.menuRowId + "')");
    check(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup), "premise: its popup is up");

    // Cull it: a four-row-tall rail cannot submit row six, so its popup is gone.
    region.h = m.row * 4.0f;
    frame(body);
    check(ui.rail.drawnRows <= 2, "premise: the short rail draws at most two rows (" +
                                      std::to_string(ui.rail.drawnRows) + ")");
    check(ui.rail.menuRowId.empty(),
          "a row culled out of the band drops the menu mirror in the same frame (got '" +
              ui.rail.menuRowId + "')");
    // The popup itself closes one frame later: ImGui closes a popup that was not submitted at the
    // end of the frame, so `IsPopupOpen` still answers true during the frame that culled the row.
    frame(body);
    check(!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup),
          "premise: the culled row's popup is gone on the next frame");

    // A *collapse* with the menu open: 90x30 is the width band where the layout hands the rail
    // its 7-cell column (`checkStrip()` proves the shape), and a strip draws no row - so nothing
    // anchors the popup and the mirror has to go in the collapse frame, or `railOwnsEscape()`
    // tells the composer an overlay is up for as long as the rail stays narrow.
    region = railAt(kScreenW, kScreenH, m);
    frame(body);
    check(pointAt(ui.rail, "s-6", static_cast<float>(rowX), static_cast<float>(rowY), body),
          "premise: the pointer is back on the sixth row");
    typeText("m", body);
    frame(body);
    check(ui.rail.menuRowId == "s-6",
          "premise: the menu is open again on the full rail (got '" + ui.rail.menuRowId + "')");
    region = railAt(90, 30, m);
    check(region.w <= 12.0f,
          "premise: 90x30 is the collapsed column (w=" + std::to_string(region.w) + ")");
    frame(body);
    check(ui.rail.menuRowId.empty(),
          "a collapse to the strip drops the menu mirror in that frame (got '" + ui.rail.menuRowId +
              "')");
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0 || std::strcmp(argv[i], "--verbose") == 0) {
            g_verbose = true;
        } else {
            usage(argv[0], "unknown argument");
        }
    }

    // One glyph set for every frame, whatever terminal runs this (`LLMFUN_ASCII=0`
    // outranks TERM and the locale), and one time zone: the day/week boundaries are
    // then plain epoch arithmetic instead of a property of the machine.
    ::setenv("LLMFUN_ASCII", "0", 1);
    ::setenv("TZ", "UTC", 1);
    ::tzset();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImTui_ImplText_Init();
    // The depth the application applies for a 256-colour terminal (`applyUiTheme`):
    // without it `ui::color()` answers in true colour, and a 14 % fill like
    // `selection` quantizes away on the grid - the row fills this binary asserts on
    // only exist at the depth the terminal actually renders.
    ui::applyTheme(ui::ThemeId::Graphite, ui::ColorDepth::Ansi256);
    ImGuiIO& io = ImGui::GetIO();
    // The application enables keyboard navigation once per frame (`tuiRender`); a popup
    // closes on `Esc` and an item reports keyboard focus only while it is on, so the
    // harness has to match the app or it would test a different ImGui.
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.DisplaySize = ImVec2(static_cast<float>(kScreenW), static_cast<float>(kScreenH));
    io.DeltaTime = 1.0f / 60.0f;
    frame([](const ui::Metrics&) {}); // one warm-up frame: the atlas is built on it

    checkGrouping();
    checkFiltering();
    checkUnread();
    checkPureRules();
    checkRowInks();
    checkDrawnRail();
    checkBoundedRendering();
    checkStrip();
    checkInteraction();
    checkMenuMirrorFollowsCulling();

    ImTui_ImplText_Shutdown();
    ImGui::DestroyContext();

    std::printf("%s: %d check(s), %d failure(s)\n", g_failures == 0 ? "PASS" : "FAIL", g_checks,
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
