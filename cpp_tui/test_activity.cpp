// test_activity.cpp
//
// The inspector's Activity tab (views/inspector.h + model/activity.h, design §4.8 / FR-15 /
// D-ACTIVITY / task 31): the user-facing operational history, and the structural promise that
// no developer data can appear in it.
//
// What this binary proves, in order:
//
//   1. the source. The panel still writes no colour literal, takes the profile from the
//      metrics, reads no display size and names no v4 type; the projection's unit reads the
//      two user-facing strings and nothing else;
//   2. the shape. `model::LogRecord` has no module, function, file, line or stack field - the
//      struct is checked from the source, so "Activity cannot leak developer data" is a
//      property of the type and not of a filter somebody has to remember;
//   3. the projection. A record carrying metadata projects to time + severity + message +
//      action, for *every* level; an error keeps its action id and payload; an unknown level
//      is info and an unknown action is none;
//   4. the promotion. Trace and debug never reach Activity; the ring is FIFO at 500; arrivals
//      while the tab is not on screen are counted and the count clears on open;
//   5. the filter and the search. `model::activityMatches` answers the level filter and a
//      case-insensitive search over the message only (never the detail, never metadata);
//   6. the tab. The empty state, the mixed-level rows, the filter and search chrome, the
//      bounded body (viewport + overscan), the quiet-by-default list (no live tail) and the
//      one-shot scroll when the tab is shown;
//   7. the goldens: empty, mixed levels, filtered.
//
// Exit codes: 0 = all checks pass; 1 = a check failed; 2 = usage error.
//
//   ./test_activity [-v] [--update] [--print]
//
// `--update` rewrites the fixtures; do that only for a deliberate rendering change, and commit
// the diff with it. The captures pin `LLMFUN_ASCII=0` and `TZ=UTC`: the row clock is local
// time, so a golden captured in another zone would differ for a reason that is not a defect.

#include "views/inspector.h"
#include "views/shell.h"

#include "tui.h"

#include "tui_api.h" // TuiLogRecord/String: the inbound record the projection consumes

#include "model/activity.h"
#include "model/settings.h"
#include "model/ui_state.h"
#include "ui/icons.h"
#include "ui/layout.h"
#include "ui/metrics.h"
#include "ui/text.h"

#include "imtui/imtui.h"

#include "imgui/imgui.h"

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

float num(float v) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.3f", double(v));
    std::string text = buffer;
    while (!text.empty() && text.back() == '0')
        text.pop_back();
    if (!text.empty() && text.back() == '.')
        text.pop_back();
    return std::string(text).empty() ? 0.0f : std::strtof(text.c_str(), nullptr);
}

std::string trimTrailingSpaces(std::string text) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.pop_back();
    return text;
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path);
    if (!in.good())
        return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

std::string readWhole(const std::string& path) {
    std::string out;
    if (!readFile(path, out))
        return std::string();
    return out;
}

bool writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path);
    if (!out.good())
        return false;
    out << text;
    return out.good();
}

// ---------------------------------------------------------------------------
// The cell grid the text backend writes (the `TScreen` pattern)
// ---------------------------------------------------------------------------

ImTui::TScreen g_screen;
int g_w = 40;
int g_h = 20;

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

    int rowOf(const std::string& needle) const {
        for (int y = 0; y < ny; ++y) {
            if (rowText(y).find(needle) != std::string::npos)
                return y;
        }
        return -1;
    }

    int colOf(int y, const std::string& needle) const {
        if (y < 0 || y >= ny)
            return -1;
        const std::string row = rowText(y);
        const std::size_t at_ = row.find(needle);
        return at_ == std::string::npos ? -1 : static_cast<int>(at_);
    }
};

Grid capture() {
    Grid grid;
    grid.nx = g_screen.nx;
    grid.ny = g_screen.ny;
    const std::size_t cells = static_cast<std::size_t>(grid.nx) * grid.ny;
    grid.ch.resize(cells);
    grid.fg.resize(cells);
    grid.bg.resize(cells);
    for (std::size_t i = 0; i < cells; ++i) {
        grid.ch[i] = g_screen.data[i].ch;
        grid.fg[i] = g_screen.data[i].fg;
        grid.bg[i] = g_screen.data[i].bg;
    }
    return grid;
}

/// One frame of the text grid at `g_w x g_h`: a decoration-free root window, the body drawing
/// inside it, then the frame through the real backend. `inject` runs *before* `NewFrame`, which
/// is when ImGui consumes its input queue.
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
    ImGui::Begin("activity-frame", nullptr,
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

const ui::Metrics& gridMetrics() {
    static const ui::Metrics m = ui::metricsFor(true, 1.0f, 1.0f);
    return m;
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

/// Exactly midnight UTC on 2026-06-21: the goldens read 00:00:00 and cannot embed the wall
/// clock, and the projection checks compare against this value instead of against themselves.
inline constexpr long long kFixtureEpochMs = 1'782'000'000'000;

/// The inbound record as the C ABI delivers it: borrowed `String` views over `storage`, which
/// must outlive the call (the projection copies during it, so a local is enough).
struct Inbound {
    std::string message;
    std::string detail;
    std::string module;
    std::string function;
    std::string file;
    std::string payload;
    TuiLogRecord record{};

    Inbound(int level, std::string msg, int action = TuiLogAction_None,
            std::string actionPayload = std::string(), std::string detailText = std::string())
        : message(std::move(msg)), detail(std::move(detailText)), module("llm.agent.pool"),
          function("processTurn"), file("/src/agent/pool.d"), payload(std::move(actionPayload)) {
        record.level = level;
        record.action = action;
        record.timestampMs = kFixtureEpochMs;
        record.line = 412;
        record.reserved = 0;
        record.message = String{message.data(), message.size()};
        record.detail = String{detail.data(), detail.size()};
        record.module = String{module.data(), module.size()};
        record.function = String{function.data(), function.size()};
        record.file = String{file.data(), file.size()};
        record.actionPayload = String{payload.data(), payload.size()};
    }
};

model::LogRecord project(int level, const std::string& message, int action = TuiLogAction_None,
                         const std::string& payload = std::string(),
                         const std::string& detail = std::string()) {
    Inbound inbound(level, message, action, payload, detail);
    return model::projectForActivity(inbound.record);
}

/// A row the Activity tab draws, as the model would have appended it.
///
/// The clock is a function of `index`, never a running counter: a static would make a golden
/// depend on how many records the checks before it happened to create.
model::LogRecord row(int level, const std::string& message, int index = 0) {
    model::LogRecord out = project(level, message);
    out.timestampMs = kFixtureEpochMs + 1000LL * index;
    return out;
}

std::deque<model::LogRecord> mixedRecords() {
    std::deque<model::LogRecord> records;
    // Short messages: the docked column is 28 cells, and a golden of truncated rows would
    // test the ellipsis instead of the tab.
    records.push_back(row(TuiLogLevel_Info, "session opened", 0));
    records.push_back(row(TuiLogLevel_Info, "model answered", 1));
    records.push_back(row(TuiLogLevel_Warn, "ui.conf readonly", 2));
    // The error carries an action id, so the goldens and the wide-panel check see the label
    // the UI owns ("Retry") and never the id itself.
    model::LogRecord failed =
        project(TuiLogLevel_Error, "read_file failed", TuiLogAction_Retry, "turn-7");
    failed.timestampMs = kFixtureEpochMs + 3000LL;
    records.push_back(failed);
    records.push_back(row(TuiLogLevel_Info, "run finished", 4));
    return records;
}

/// A panel state that is up and showing Activity, sized by the real layout rule.
ui::Rect panelRect(int w, int h) {
    ui::LayoutPrefs prefs;
    prefs.inspectorOpen = true;
    const ui::ShellLayout layout =
        views::shellLayoutFor(prefs, ImVec2(static_cast<float>(w), static_cast<float>(h)),
                              /*inspectorAuto=*/false);
    return layout.inspector;
}

views::InspectorState openActivity() {
    views::InspectorState state;
    state.open = true;
    state.tab = model::InspectorTab::Activity;
    return state;
}

/// Draw the panel for `frames` frames and answer the grid of the last one.
Grid drawPanel(views::InspectorState& state, const std::deque<model::LogRecord>& records,
               int frames = 2) {
    const ui::Rect rect = panelRect(g_w, g_h);
    Grid grid;
    for (int i = 0; i < frames; ++i) {
        grid = frame([&](const ui::Metrics& m) {
            views::renderInspector(state, {}, rect, m, /*acceptKeys=*/true, records);
        });
    }
    return grid;
}

// ---------------------------------------------------------------------------
// 1. The source
// ---------------------------------------------------------------------------

void checkSource() {
    const std::string panel = readWhole(TEST_ACTIVITY_SOURCE);
    check(!panel.empty(), "views/inspector.cpp is readable");
    check(panel.find("tuiIsTextGrid") == std::string::npos,
          "the tab takes the profile from the metrics, never from tuiIsTextGrid()");
    check(panel.find("ImVec4(") == std::string::npos, "the tab writes no colour literal (D-THEME)");
    check(panel.find("DisplaySize") == std::string::npos,
          "the tab reads no display size (a view is a geometry taker)");
    check(panel.find("ChatMessage") == std::string::npos &&
              panel.find("RenderGroup") == std::string::npos,
          "the tab names no v4 type: it draws `model::LogRecord` values (D-ADAPTER)");
    check(panel.find("#include \"tui.h\"") == std::string::npos,
          "the panel does not include tui.h");
    // Quiet by default: the only scroll pin is the one-shot latch, so the call site is guarded
    // by `activityScrollToEnd` and never runs per frame (D-FOLLOW's rule).
    const std::size_t pin = panel.find("SetScrollHereY");
    check(panel.find("SetScrollHereY") == std::string::npos ||
              panel.rfind("activityScrollToEnd", panel.find("SetScrollHereY")) != std::string::npos,
          "the list is pinned only by the one-shot latch, never every frame");

    // ... and `End` is the press that owes it the catch-up: the helper must be called from the
    // ScrollEnd case, or the latch has no arming path in a real frame (`views/shell.cpp` arms it
    // on a tab switch, the filter press in `views/inspector.cpp`, `End` here).
    const std::string tuiSource = readWhole(TEST_ACTIVITY_TUI_SOURCE);
    const std::size_t scrollEnd = tuiSource.find("case model::Action::ScrollEnd:");
    const std::size_t nextCase = scrollEnd == std::string::npos
                                     ? std::string::npos
                                     : tuiSource.find("case model::Action::", scrollEnd + 1);
    check(scrollEnd != std::string::npos && nextCase != std::string::npos &&
              tuiSource.find("scrollActivityToNewest", scrollEnd) < nextCase,
          "`End` (`Action::ScrollEnd`) arms the Activity catch-up");

    const std::string modelSource = readWhole(TEST_ACTIVITY_MODEL_SOURCE);
    check(!modelSource.empty(), "model/activity.cpp is readable");
    check(modelSource.find("out.module") == std::string::npos &&
              modelSource.find("out.file") == std::string::npos &&
              modelSource.find("out.function") == std::string::npos &&
              modelSource.find("out.line") == std::string::npos,
          "the projection copies no developer field (it has nowhere to put one)");
}

// ---------------------------------------------------------------------------
// 2. The shape of the Activity record
// ---------------------------------------------------------------------------

void checkShape() {
    // The struct is checked from the *header*: a field that is not declared cannot be drawn,
    // which is what makes "no developer data in Activity" structural (R-07).
    const std::string header = readWhole(TEST_ACTIVITY_HEADER_SOURCE);
    check(!header.empty(), "model/activity.h is readable (shape premise)");
    const std::size_t structAt = header.find("struct LogRecord {");
    check(structAt != std::string::npos, "the Activity record is declared in the model header");
    const std::size_t structEnd =
        structAt == std::string::npos ? std::string::npos : header.find("};", structAt);
    check(structAt != std::string::npos && structEnd != std::string::npos,
          "the Activity record's declaration is a complete struct");
    std::string fields;
    if (structAt != std::string::npos && structEnd != std::string::npos) {
        // Only the declarations: the struct's own comments are prose (they say "one line"
        // about the message), and a shape check that read them would fail on English.
        const std::string block = header.substr(structAt, structEnd - structAt);
        std::size_t at = 0;
        while (at < block.size()) {
            const std::size_t end = block.find('\n', at);
            const std::string line =
                block.substr(at, end == std::string::npos ? std::string::npos : end - at);
            std::size_t first = line.find_first_not_of(" \t");
            const bool comment = first != std::string::npos && line.compare(first, 2, "//") == 0;
            if (!comment)
                fields += line + "\n";
            if (end == std::string::npos)
                break;
            at = end + 1;
        }
    }
    for (const char* forbidden : {"module", "function", "file", "line", "stack"}) {
        check(fields.find(forbidden) == std::string::npos,
              std::string("the Activity record has no `") + forbidden +
                  "` field: developer data cannot reach the tab (R-07)");
    }

    model::LogRecord record;
    record.timestampMs = 1;
    record.level = TuiLogLevel_Warn;
    record.message = "m";
    record.detail = "d";
    record.action = model::LogAction::Details;
    record.actionPayload = "p";
    check(record.message == "m" && record.detail == "d" && record.actionPayload == "p",
          "the Activity record holds time, severity, message, detail and the action");
    check(model::kActivityRecordsMax == 500, "the ring is the design's 500 rows");
}

// ---------------------------------------------------------------------------
// 3. The projection
// ---------------------------------------------------------------------------

void checkProjection() {
    // Every level projects: the projection is defined for all of them (the *promotion* rule is
    // what keeps trace and debug out of Activity, checked below).
    for (int level : {static_cast<int>(TuiLogLevel_Trace), static_cast<int>(TuiLogLevel_Debug),
                      static_cast<int>(TuiLogLevel_Info), static_cast<int>(TuiLogLevel_Warn),
                      static_cast<int>(TuiLogLevel_Error), 42, -1}) {
        Inbound inbound(level, "something happened");
        const model::LogRecord out = model::projectForActivity(inbound.record);
        check(out.message == "something happened",
              "the message survives projection (level " + std::to_string(level) + ")");
        check(out.timestampMs == kFixtureEpochMs,
              "the clock survives projection (the fixture's value, not 0 == 0)");
        check(out.level == model::activityLevel(level), "the severity is the clamped one");
    }

    // The metadata cannot come out: the projection has no field for it, and none of the strings
    // it does carry contain a module, function, file or line.
    Inbound inbound(TuiLogLevel_Error, "tool read_file failed", TuiLogAction_Retry, "turn-7",
                    "stack: at pool.d:412");
    const model::LogRecord out = model::projectForActivity(inbound.record);
    const std::string everything = out.message + out.detail + out.actionPayload;
    check(everything.find("llm.agent.pool") == std::string::npos,
          "no module name reaches Activity");
    check(everything.find("processTurn") == std::string::npos, "no function name reaches Activity");
    check(everything.find("/src/agent/pool.d") == std::string::npos,
          "no file path reaches Activity");
    check(out.message.find("412") == std::string::npos,
          "no line number reaches the message Activity draws (the detail keeps it for the"
          " Raw Log)");

    // An error keeps its action id and payload - that is what makes the row actionable later.
    check(out.action == model::LogAction::Retry, "an error keeps its action id");
    check(out.actionPayload == "turn-7", "an error keeps its action payload");
    check(out.detail == "stack: at pool.d:412", "the detail is carried for the Raw Log");

    // Unknown values off the wire are folded, never guessed.
    check(project(42, "x").level == TuiLogLevel_Info, "an unknown level is info");
    check(project(TuiLogLevel_Trace, "x").level == TuiLogLevel_Info, "trace is info in Activity");
    check(project(TuiLogLevel_Debug, "x").level == TuiLogLevel_Info, "debug is info in Activity");
    check(project(TuiLogLevel_Warn, "x").level == TuiLogLevel_Warn, "warn keeps its severity");
    check(project(TuiLogLevel_Error, "x").level == TuiLogLevel_Error, "error keeps its severity");
    Inbound unknownAction(TuiLogLevel_Info, "x", 99);
    check(model::projectForActivity(unknownAction.record).action == model::LogAction::None,
          "an unknown action id is none, not a guessed button");
    Inbound details(TuiLogLevel_Info, "x", TuiLogAction_Details);
    check(model::projectForActivity(details.record).action == model::LogAction::Details,
          "Details survives the projection");

    // Null strings are empty strings, never a crash.
    TuiLogRecord empty{};
    empty.level = TuiLogLevel_Info;
    const model::LogRecord blank = model::projectForActivity(empty);
    check(blank.message.empty() && blank.detail.empty() && blank.actionPayload.empty(),
          "a record with no strings projects to empty strings");
    check(blank.timestampMs == 0, "a record with no clock keeps 0 (the ABI stamps it)");

    // The severity words: the three Activity draws, and the short form the 28-cell row needs.
    check(std::string(model::activityLevelWord(TuiLogLevel_Info)) == "info", "info's word");
    check(std::string(model::activityLevelWord(TuiLogLevel_Warn)) == "warn", "warn's word");
    check(std::string(model::activityLevelWord(TuiLogLevel_Error)) == "error", "error's word");
}

// ---------------------------------------------------------------------------
// 4. Promotion, the ring and the badge
// ---------------------------------------------------------------------------

void checkRing() {
    check(model::activityShowsLevel(TuiLogLevel_Info), "info is promoted");
    check(model::activityShowsLevel(TuiLogLevel_Warn), "warn is promoted");
    check(model::activityShowsLevel(TuiLogLevel_Error), "error is promoted");
    check(!model::activityShowsLevel(TuiLogLevel_Trace), "trace is not promoted");
    check(!model::activityShowsLevel(TuiLogLevel_Debug), "debug is not promoted");
    check(!model::activityShowsLevel(42), "an unknown level is not promoted");

    // The rule is *applied*, not merely defined (task-31 review C1): the predicate is wired
    // into the one appender, so a trace or debug record never enters the ring and never moves
    // the badge - the mistake the review found was exactly this gap.
    {
        model::UiState gated;
        model::appendActivity(gated, project(TuiLogLevel_Trace, "trace noise"), TuiLogLevel_Trace,
                              /*visible=*/false);
        model::appendActivity(gated, project(TuiLogLevel_Debug, "debug noise"), TuiLogLevel_Debug,
                              /*visible=*/false);
        check(gated.activity.empty() && gated.unreadActivity == 0,
              "trace and debug never reach Activity (promotion is applied by appendActivity)");
        model::appendActivity(gated, project(42, "unknown level"), 42, /*visible=*/false);
        check(gated.activity.empty(), "an unknown level is not promoted either");
        model::appendActivity(gated, project(TuiLogLevel_Info, "kept"), TuiLogLevel_Info,
                              /*visible=*/false);
        check(gated.activity.size() == 1 && gated.unreadActivity == 1,
              "info is promoted (and counted)");
        model::appendActivity(gated, project(TuiLogLevel_Error, "kept too"), TuiLogLevel_Error,
                              /*visible=*/false);
        check(gated.activity.size() == 2, "an error is promoted");
    }

    model::UiState state;
    check(state.activity.empty() && state.unreadActivity == 0, "a fresh state has no Activity");

    model::appendActivity(state, row(TuiLogLevel_Info, "one", 0), TuiLogLevel_Info,
                          /*visible=*/false);
    check(state.activity.size() == 1, "the first record is appended");
    check(state.unreadActivity == 1, "a record that arrives unseen is unread");
    model::appendActivity(state, row(TuiLogLevel_Error, "two", 1), TuiLogLevel_Error,
                          /*visible=*/true);
    check(state.activity.size() == 2, "a record that arrives while the tab is up is appended");
    check(state.unreadActivity == 1, "and it is already read (no unread count)");
    model::clearActivityUnread(state);
    check(state.unreadActivity == 0, "showing the tab clears the count");

    // A dropped raw error produces exactly one Activity error row: one arrival, one row.
    const std::size_t before = state.activity.size();
    model::appendActivity(state, project(TuiLogLevel_Error, "log ring dropped 1 record"),
                          TuiLogLevel_Error, /*visible=*/false);
    check(state.activity.size() == before + 1, "one dropped-record notice is one row");
    check(state.activity.back().level == TuiLogLevel_Error, "and it is an error row");

    // FIFO at 500: the newest stay, the oldest leave, and the unread count is arrivals.
    model::UiState ring;
    for (int i = 0; i < 520; ++i)
        model::appendActivity(ring, row(TuiLogLevel_Info, "record " + std::to_string(i), i % 50),
                              TuiLogLevel_Info, false);
    check(ring.activity.size() == model::kActivityRecordsMax, "the ring stops at 500 rows");
    check(ring.activity.front().message == "record 20", "the oldest rows were evicted (FIFO)");
    check(ring.activity.back().message == "record 519", "the newest row is the last one");
    check(ring.unreadActivity == 520, "the unread count counts arrivals, not the ring's size");
}

// ---------------------------------------------------------------------------
// 5. The filter and the search
// ---------------------------------------------------------------------------

void checkFilterAndSearch() {
    const model::LogRecord info = project(TuiLogLevel_Info, "Session opened");
    const model::LogRecord warn = project(TuiLogLevel_Warn, "ui.conf is read-only");
    const model::LogRecord error =
        project(TuiLogLevel_Error, "tool read_file failed", TuiLogAction_Retry, "turn-7",
                "detail mentions the word session too");

    check(model::activityMatches(info, model::ActivityFilter::All, ""), "all keeps info");
    check(model::activityMatches(warn, model::ActivityFilter::All, ""), "all keeps warn");
    check(model::activityMatches(error, model::ActivityFilter::All, ""), "all keeps error");
    check(!model::activityMatches(warn, model::ActivityFilter::Info, ""),
          "the info filter hides a warning");
    check(model::activityMatches(warn, model::ActivityFilter::Warn, ""),
          "the warn filter keeps a warning");
    check(!model::activityMatches(info, model::ActivityFilter::Error, ""),
          "the error filter hides info");
    check(model::activityMatches(error, model::ActivityFilter::Error, ""),
          "the error filter keeps an error");

    // The search is over the message only: the detail's text must not match.
    check(model::activityMatches(info, model::ActivityFilter::All, "session"),
          "the search finds a message, case-insensitively");
    check(!model::activityMatches(error, model::ActivityFilter::All, "session"),
          "the search never looks at the detail text");
    check(!model::activityMatches(info, model::ActivityFilter::All, "read_file"),
          "the search never looks at another row's message");
    check(model::activityMatches(info, model::ActivityFilter::All, "   "),
          "a blanks-only search is no search at all");
    check(!model::activityMatches(info, model::ActivityFilter::All, "no such text"),
          "a search with no match hides the row");
    // The filter and the search are one answer: an error that does not match the text is out
    // even when the level would pass.
    check(!model::activityMatches(error, model::ActivityFilter::Error, "nothing"),
          "the filter and the search must both pass");
}

// ---------------------------------------------------------------------------
// 6. The tab on the grid
// ---------------------------------------------------------------------------

void checkEmptyState() {
    g_w = 40;
    g_h = 20;
    views::InspectorState state = openActivity();
    const Grid grid = drawPanel(state, {});
    check(grid.hasText("Nothing to report yet"),
          "an empty Activity tab says so instead of drawing a blank panel");
    check(grid.hasText("all") && grid.hasText("info") && grid.hasText("warn") &&
              grid.hasText("error"),
          "the four severity filters are drawn");
    check(grid.hasText("Search"), "the search field is drawn with its hint");
    check(!grid.hasText("[]"), "no empty action brackets are drawn");
}

void checkRows() {
    g_w = 40;
    g_h = 20;
    views::InspectorState state = openActivity();
    const std::deque<model::LogRecord> records = mixedRecords();
    const ui::Rect rect = panelRect(g_w, g_h);
    const views::InspectorParts parts =
        views::inspectorMeasure(state, {}, rect, gridMetrics(), records);
    check(parts.bodyRows == records.size(), "every record is one row (no wrapping)");
    check(parts.activityRows.size() == records.size(), "the filter keeps all five rows");
    check(parts.filterAll.w > 0.0f && parts.filterInfo.w > 0.0f && parts.filterWarn.w > 0.0f &&
              parts.filterError.w > 0.0f,
          "all four chips fit the docked column");
    const float chipsRight = parts.filterError.right();
    check(chipsRight <= rect.right() - gridMetrics().pad1 + 0.001f,
          "the chips stay inside the panel");
    check(parts.search.w > 0.0f, "the search field has the body's width");

    const Grid grid = drawPanel(state, records);
    check(grid.hasText("info"), "the severity word is drawn (never colour alone)");
    check(grid.hasText("warn"), "a warning row prints its word");
    check(grid.hasText("error"), "an error row prints its word");
    check(grid.hasText("read_file fail"), "the message is drawn (cut with the profile's ellipsis)");
    check(grid.hasText("session opened") && grid.hasText("run finished"),
          "the rows are on screen, oldest first");
    check(!grid.hasText("llm.agent.pool") && !grid.hasText("processTurn") &&
              !grid.hasText("pool.d"),
          "no module, function or file text reaches the screen");
    check(!grid.hasText("412"), "no line number reaches the screen");
    // The clock is the row's first column, and the fixture's stamp is a fixed instant.
    const int rowWithMessage = grid.rowOf("read_file fail");
    check(rowWithMessage >= 0 && grid.rowText(rowWithMessage).find("00:00:0") != std::string::npos,
          "the row carries a clock (UTC, pinned by this harness)");
}

void checkBounded() {
    g_w = 40;
    g_h = 20;
    views::InspectorState state = openActivity();
    std::deque<model::LogRecord> records;
    for (int i = 0; i < static_cast<int>(model::kActivityRecordsMax); ++i)
        records.push_back(row(TuiLogLevel_Info, "record " + std::to_string(i), i % 50));

    drawPanel(state, records);
    const ui::Rect rect = panelRect(g_w, g_h);
    const views::InspectorParts parts =
        views::inspectorMeasure(state, {}, rect, gridMetrics(), records);
    const float rowH = gridMetrics().row;
    const float bound = std::ceil(parts.body.h / rowH) + 2.0f;
    check(static_cast<float>(state.drawnRows) <= bound,
          "the body draws its viewport plus one row of overscan, never the whole ring (" +
              std::to_string(state.drawnRows) + " of " + std::to_string(records.size()) + ")");
    check(parts.bodyRows == model::kActivityRecordsMax,
          "the row count is the ring's, not the window's");
}

void checkQuietAndBadge() {
    g_w = 40;
    g_h = 20;
    views::InspectorState state = openActivity();
    std::deque<model::LogRecord> records;
    for (int i = 0; i < 60; ++i)
        records.push_back(row(TuiLogLevel_Info, "record " + std::to_string(i), i % 50));

    // The one-shot latch is on for the first frames: the newest record is what the user sees.
    const Grid shown = drawPanel(state, records);
    check(!state.activityScrollToEnd, "the one-shot scroll latch is spent after one frame");
    check(shown.hasText("record 59"), "opening the tab shows the newest record");

    // Quiet by default: a record that arrives afterwards must not move the view.
    // (The message is short on purpose: the docked body is 26 cells, so a long one is cut
    // after the row's clock/glyph/level head and would fail a text match for the wrong reason.)
    records.push_back(row(TuiLogLevel_Error, "fresh failure", 59));
    const Grid after = drawPanel(state, records);
    check(after.rowText(0) == shown.rowText(0) && after.rowText(1) == shown.rowText(1),
          "a new record does not scroll the list (quiet by default)");
    check(!after.hasText("fresh"),
          "the new row is out of the window and the view stayed where the user left it");

    // `End` is the catch-up (task 31, option 3): the list has no follower, so the one-shot latch
    // is what the press owes it - and only from the tab that owns the list.
    views::InspectorState agentsTab = state;
    agentsTab.tab = model::InspectorTab::Agents;
    check(!views::scrollActivityToNewest(agentsTab),
          "`End` does not arm the Activity catch-up from another tab");
    views::InspectorState closed = state;
    closed.open = false;
    check(!views::scrollActivityToNewest(closed),
          "`End` does not arm the catch-up of a closed panel's list");
    check(views::scrollActivityToNewest(state),
          "`End` arms the catch-up while the Activity tab is on screen");
    const Grid caughtUp = drawPanel(state, records);
    check(!state.activityScrollToEnd,
          "the catch-up is one shot too (spent by the frame it serves)");
    check(caughtUp.hasText("fresh"),
          "`End` brings the newest record back into view (a catch-up is owed, not a position"
          " test)");

    // ... and the badge is the projection's count, not the ring's size: the frame layer reads
    // it (deriveInspector) and clears it when the tab is on screen. A harness that drives the
    // model directly sees exactly that contract.
    model::UiState ui;
    model::appendActivity(ui, project(TuiLogLevel_Warn, "ui.conf is read-only"), TuiLogLevel_Warn,
                          false);
    model::appendActivity(ui, project(TuiLogLevel_Error, "tool read_file failed"),
                          TuiLogLevel_Error, false);
    check(ui.unreadActivity == 2, "two unseen records are two unread");
    model::clearActivityUnread(ui);
    check(ui.unreadActivity == 0, "showing the tab reads them");
}

void checkFilteredTab() {
    g_w = 40;
    g_h = 20;
    views::InspectorState state = openActivity();
    const std::deque<model::LogRecord> records = mixedRecords();

    state.activityFilter = model::ActivityFilter::Error;
    const ui::Rect rect = panelRect(g_w, g_h);
    const views::InspectorParts parts =
        views::inspectorMeasure(state, {}, rect, gridMetrics(), records);
    check(parts.activityRows.size() == 1, "the error filter leaves the one error row");
    const Grid grid = drawPanel(state, records);
    check(grid.hasText("read_file fail"), "the filtered row is drawn");
    check(!grid.hasText("model answered"), "the info rows are gone");

    // A search that matches nothing says so - "nothing to report yet" would be a lie.
    state.activityFilter = model::ActivityFilter::All;
    std::snprintf(state.activitySearch.data(), state.activitySearch.size(), "%s", "zzz");
    const Grid none = drawPanel(state, records);
    check(none.hasText("No records match"), "a search with no match says so");
    check(!none.hasText("Nothing to report yet"), "and does not claim the run has no history");

    // A search that matches: one row, and the query is not case-sensitive.
    std::snprintf(state.activitySearch.data(), state.activitySearch.size(), "%s", "SESSION");
    const Grid one = drawPanel(state, records);
    check(one.hasText("session opened"), "the search finds the message");
    check(!one.hasText("run finished"), "and hides the rows it does not match");
}

/// Click the centre of a chip the measure placed, and report the filter afterwards.
model::ActivityFilter clickChip(views::InspectorState& state,
                                const std::deque<model::LogRecord>& records,
                                model::ActivityFilter which) {
    const ui::Rect rect = panelRect(g_w, g_h);
    const views::InspectorParts parts =
        views::inspectorMeasure(state, {}, rect, gridMetrics(), records);
    views::InspectorPart part;
    switch (which) {
    case model::ActivityFilter::All:
        part = parts.filterAll;
        break;
    case model::ActivityFilter::Info:
        part = parts.filterInfo;
        break;
    case model::ActivityFilter::Warn:
        part = parts.filterWarn;
        break;
    case model::ActivityFilter::Error:
        part = parts.filterError;
        break;
    }
    if (part.w <= 0.0f)
        return state.activityFilter;
    const float x = part.x + part.w * 0.5f;
    const float y = part.y + part.h * 0.5f;
    // A click is a sequence and ImGui trickles one event per frame: move, press, release.
    frame([&](const ui::Metrics& m) { views::renderInspector(state, {}, rect, m, true, records); },
          [x, y](ImGuiIO& io) { io.AddMousePosEvent(x, y); });
    frame([&](const ui::Metrics& m) { views::renderInspector(state, {}, rect, m, true, records); },
          [x, y](ImGuiIO& io) {
              io.AddMousePosEvent(x, y);
              io.AddMouseButtonEvent(0, true);
          });
    frame([&](const ui::Metrics& m) { views::renderInspector(state, {}, rect, m, true, records); },
          [x, y](ImGuiIO& io) {
              io.AddMousePosEvent(x, y);
              io.AddMouseButtonEvent(0, false);
          });
    frame([&](const ui::Metrics& m) { views::renderInspector(state, {}, rect, m, true, records); });
    return state.activityFilter;
}

void checkChipsAreControls() {
    g_w = 40;
    g_h = 20;
    views::InspectorState state = openActivity();
    const std::deque<model::LogRecord> records = mixedRecords();
    drawPanel(state, records);
    check(clickChip(state, records, model::ActivityFilter::Error) == model::ActivityFilter::Error,
          "a press on `error` filters to errors");
    check(clickChip(state, records, model::ActivityFilter::Warn) == model::ActivityFilter::Warn,
          "a press on `warn` filters to warnings");
    check(clickChip(state, records, model::ActivityFilter::All) == model::ActivityFilter::All,
          "a press on `all` clears the filter");
    check(clickChip(state, records, model::ActivityFilter::Info) == model::ActivityFilter::Info,
          "a second press on another chip moves the filter again");
}

/// A body too short for the search field must not keep filtering by a query nobody can see or
/// clear (task-31 review M3). The tab gives the field up before the list; the query goes with
/// it.
void checkShortBodyDropsTheQuery() {
    views::InspectorState state = openActivity();
    const std::deque<model::LogRecord> records = mixedRecords();
    std::snprintf(state.activitySearch.data(), state.activitySearch.size(), "%s", "zzz");

    // Three rows of panel: the strip, the filter row and one list row - no room for the field.
    const ui::Rect shortRegion{0.0f, 0.0f, 28.0f, 3.0f, gridMetrics().textGrid};
    const views::InspectorParts parts =
        views::inspectorMeasure(state, {}, shortRegion, gridMetrics(), records);
    check(parts.search.empty(), "premise: the short body has no search field");
    check(parts.activityRows.size() == records.size(),
          "a leftover query does not filter when its field is not drawn");
    check(!parts.activityFilteredEmpty, "and the body does not claim there are no matches");

    // The same query *does* filter when the field is there.
    const ui::Rect tallRegion{0.0f, 0.0f, 28.0f, 20.0f, gridMetrics().textGrid};
    const views::InspectorParts tall =
        views::inspectorMeasure(state, {}, tallRegion, gridMetrics(), records);
    check(!tall.search.empty(), "the tall body has the field");
    check(tall.activityRows.empty() && tall.activityFilteredEmpty,
          "and the query filters (with a control to clear)");
}

void checkWideAction() {
    // A 72-cell panel has room for the action beside the message: the row prints the UI's
    // label, never the id D sent, and only when the record carries one.
    g_w = 80;
    g_h = 24;
    views::InspectorState state = openActivity();
    const std::deque<model::LogRecord> records = mixedRecords();
    const Grid grid = drawPanel(state, records);
    check(grid.hasText("[Retry]"), "a wide panel draws the action's label");
    check(grid.hasText("read_file failed"), "and the message it belongs to");
    check(!grid.hasText("turn-7"),
          "the action's payload is never drawn (it is an id, not a label)");
    check(!grid.hasText("[Details]"), "a record without that action draws no such label");
}

/// The frame's own badge rule (D-UNREAD), end to end: a record that arrives while the tab is
/// not the one on screen shows in the strip, and showing the tab reads it. This is the half
/// the model checks above cannot see - `deriveInspector()` owns it.
void checkBadgeThroughTheFrame() {
    const int w = 100;
    const int h = 30;
    {
        tui::TuiState state;
        state.ui.prefs.inspectorOpen = 1;
        state.ui.prefs.tab = model::InspectorTab::Agents;
        for (int i = 0; i < 3; ++i)
            model::appendActivity(state.ui, row(model::kActivityLevelWarn, "unseen", i),
                                  TuiLogLevel_Warn,
                                  /*visible=*/false);
        // Two frames: the first derives the panel's geometry, the second draws the badge the
        // first one's derivation produced (the frame layer's one-frame settle, as everywhere).
        g_w = w;
        g_h = h;
        frame([&](const ui::Metrics&) {
            views::renderShell(state, ImVec2(static_cast<float>(w), static_cast<float>(h)));
        });
        const Grid grid = frame([&](const ui::Metrics&) {
            views::renderShell(state, ImVec2(static_cast<float>(w), static_cast<float>(h)));
        });
        check(state.inspector.unreadActivity == 3, "three unseen records are three unread");
        check(state.ui.unreadActivity == 3, "and the projection's own count agrees");
        // The badge is the projection's count drawn beside the Activity label - assert the
        // mark, not a bare digit a clock could satisfy (task-31 review M7).
        const views::InspectorParts badgeParts = views::inspectorMeasure(
            state.inspector, state.ui.agents, panelRect(w, h), gridMetrics(), state.ui.activity);
        // `\u2022` is the unread mark (the table's `Icon::Unread`); the adjacent literal keeps
        // the count out of the escape (`\xa23` would be parsed as one hex escape).
        check(badgeParts.badgeText == "\u2022"
                                      "3",
              "the badge is the count beside the Activity label (" + badgeParts.badgeText + ")");
        check(grid.hasText(badgeParts.badgeText), "and the strip draws it");

        // Showing the tab reads them: the count is zero and the strip has no badge left.
        state.ui.prefs.tab = model::InspectorTab::Activity;
        frame([&](const ui::Metrics&) {
            views::renderShell(state, ImVec2(static_cast<float>(w), static_cast<float>(h)));
        });
        check(state.ui.unreadActivity == 0, "showing Activity clears the projection's count");
        check(state.inspector.unreadActivity == 0, "and the badge it draws");
    }
}

// ---------------------------------------------------------------------------
// 7. The goldens
// ---------------------------------------------------------------------------

struct Scene {
    const char* name;
    int w;
    int h;
    int filter;
    const char* search;
    bool records;
};

std::vector<Scene> scenes() {
    return {
        {"activity-empty", 40, 20, 0, "", false},
        {"activity-mixed", 40, 20, 0, "", true},
        {"activity-filtered", 40, 20, 3, "read", true},
    };
}

std::string goldenText(const Scene& scene) {
    g_w = scene.w;
    g_h = scene.h;
    views::InspectorState state = openActivity();
    state.activityFilter = static_cast<model::ActivityFilter>(scene.filter);
    if (scene.search != nullptr && scene.search[0] != '\0')
        std::snprintf(state.activitySearch.data(), state.activitySearch.size(), "%s", scene.search);
    const std::deque<model::LogRecord> records =
        scene.records ? mixedRecords() : std::deque<model::LogRecord>();
    const ui::Rect rect = panelRect(scene.w, scene.h);
    // Two frames, the second captured: the first consumes the one-shot scroll and whatever
    // active id the shared context inherited from the checks above.
    Grid grid;
    for (int i = 0; i < 2; ++i) {
        grid = frame([&](const ui::Metrics& m) {
            views::renderInspector(state, {}, rect, m, /*acceptKeys=*/true, records);
        });
    }
    const views::InspectorParts parts =
        views::inspectorMeasure(state, {}, rect, gridMetrics(), records);

    std::string text = "name: ";
    text += scene.name;
    text += "\nscreen: " + std::to_string(scene.w) + "x" + std::to_string(scene.h);
    text += "\npanel: x=" + std::to_string(static_cast<int>(rect.x)) +
            " y=" + std::to_string(static_cast<int>(rect.y)) +
            " w=" + std::to_string(static_cast<int>(rect.w)) +
            " h=" + std::to_string(static_cast<int>(rect.h));
    text += "\nfilter: " + std::to_string(static_cast<int>(state.activityFilter));
    text += " search: '" + std::string(state.activitySearch.data()) + "'";
    text += "\nparts: body=" + std::to_string(static_cast<int>(parts.body.w)) + "x" +
            std::to_string(static_cast<int>(parts.body.h)) +
            " rows=" + std::to_string(parts.bodyRows) +
            " matched=" + std::to_string(parts.activityRows.size()) +
            " drawn=" + std::to_string(state.drawnRows);
    text += "\nlabels: all='" + parts.filterAllText + "' info='" + parts.filterInfoText +
            "' warn='" + parts.filterWarnText + "' error='" + parts.filterErrorText + "' empty='" +
            parts.emptyText + "'";
    text += "\ntext:\n";
    for (int y = 0; y < grid.ny; ++y) {
        text += trimTrailingSpaces(grid.rowText(y));
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

void checkGoldens(bool update) {
    const std::string dir = TEST_ACTIVITY_GOLDEN_DIR;
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
            check(true, std::string("the ") + scene.name + " golden matches");
            continue;
        }
        std::size_t line = 1;
        std::size_t at_ = 0;
        while (at_ < expected.size() && at_ < text.size() && expected[at_] == text[at_]) {
            line += expected[at_] == '\n' ? 1 : 0;
            ++at_;
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
        else if (std::strcmp(argv[i], "--print") == 0)
            g_print = true;
        else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0)
            usage(argv[0], "print every check with -v, rewrite the goldens with --update, "
                           "dump the grids with --print");
        else
            usage(argv[0], "unknown argument");
    }

    // One glyph set and one clock for every frame, whatever machine runs this: `LLMFUN_ASCII=0`
    // outranks TERM and the locale, and the row clock is local time, so the zone is pinned to
    // UTC before any capture.
    ::setenv("LLMFUN_ASCII", "0", 1);
    ::setenv("TZ", "UTC", 1);
    ::tzset();

    ImGui::CreateContext();
    ImTui_ImplText_Init();

    checkSource();
    checkShape();
    checkProjection();
    checkRing();
    checkFilterAndSearch();
    checkEmptyState();
    checkRows();
    checkBounded();
    checkQuietAndBadge();
    checkFilteredTab();
    checkChipsAreControls();
    checkWideAction();
    checkShortBodyDropsTheQuery();
    checkBadgeThroughTheFrame();
    checkGoldens(update);

    ImGui::DestroyContext();

    if (g_failures == 0) {
        std::printf("test_activity: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "test_activity: %d of %d checks failed\n", g_failures, g_checks);
    return 1;
}
