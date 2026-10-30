// test_toast.cpp
//
// Task 45 (design §5.25, FR-29): the feedback queue and the toast primitive.
//
// What is pinned here, case by case:
//
//   1. the lifetimes     the task's table, per profile: info 4 s / success 8 s in the pixel
//                        profile, both 2 s on the grid, and warn/error persist (`0`, no clock can
//                        retire them). A raised item is stamped from the *injected* clock, so a
//                        lifetime is a number this test reads instead of a duration it waits for.
//   2. the queue rules   a repeat of the newest item refreshes it instead of stacking a second
//                        one; the depth is the profile's (3 pixel / 1 grid); a transient never
//                        evicts an unacknowledged warning, and on the grid it is dropped instead;
//                        `expireToasts` drops exactly the items whose time is up; dismissing takes
//                        the oldest and answers whether there was one.
//   3. the measure       a toast grows to its content and to the caller's wish (the design's
//                        360 px card), rounds to whole units, and answers one row per line it
//                        carries plus the paddings - in both profiles.
//   4. mixed units       a rect whose `cells` flag disagrees with the metrics paints nothing, in
//                        the grid *and* in the vertex list: a hand-built rect must not draw a
//                        plausible lie in the wrong coordinate space.
//   5. render == size    the card is drawn at exactly its measured rect and paints nothing
//                        outside it, while the title, the detail, the mark and the dismiss cross
//                        are all inside.
//   6. the controls      the dismiss cross and the (optional) action answer a press at their own
//                        rects and nowhere else, a card with no action answers `false` to the
//                        action query, and a rect no card was drawn at answers nothing.
//   7. the stack         `toastStackRects` puts the newest card at the bottom, right-aligned, one
//                        `pad1` apart, inside the area it was given, at most three deep, and draws
//                        nothing at all on the text grid (the flash is the grid's presentation).
//   8. the look          one mapping per severity (`views/feedback.h`), and a persistent item's
//                        card is the one that carries the `Dismiss` action.
//
// Exit codes: 0 = every check passed; 1 = at least one failed; 2 = usage error.
// Run from the build dir:  ./test_toast [-v]
//
// The text grid is driven through the real ImTui text backend (no ncurses, no PTY, no display),
// like `./test_primitives`; `LLMFUN_ASCII=0` is pinned at start-up so the glyph column cannot
// depend on the terminal the test runs in.

#include "imtui/imtui.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include "model/toast.h"
#include "model/ui_state.h"
#include "ui/icons.h"
#include "ui/metrics.h"
#include "ui/primitives.h"
#include "ui/text.h"
#include "ui/tokens.h"
#include "views/feedback.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace ui = llmfun::tui::ui;
namespace model = llmfun::tui::model;
namespace views = llmfun::tui::views;

namespace {

int g_failures = 0;
int g_checks = 0;
bool g_verbose = false;

constexpr int kScreenW = 60;
constexpr int kScreenH = 12;

void check(bool condition, const std::string& what) {
    ++g_checks;
    if (condition) {
        if (g_verbose)
            std::fprintf(stderr, "ok   %s\n", what.c_str());
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

std::string sizeText(ui::Size s) { return num(s.w) + "x" + num(s.h); }

ui::Metrics gridMetrics() { return ui::metricsFor(true, 1.0f, 1.0f); }
ui::Metrics guiMetrics() { return ui::metricsFor(false, 1.0f, 1.0f); }

ui::Rect rectCells(float x, float y, float w, float h) {
    ui::Rect r;
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    r.cells = true;
    return r;
}

ui::Rect rectPixels(float x, float y, float w, float h) {
    ui::Rect r = rectCells(x, y, w, h);
    r.cells = false;
    return r;
}

// ---------------------------------------------------------------------------
// The cell grid the text backend writes
// ---------------------------------------------------------------------------

ImTui::TScreen g_screen;

struct Grid {
    int nx = 0;
    int ny = 0;
    std::vector<uint32_t> ch;

    uint32_t at(int x, int y) const {
        if (x < 0 || y < 0 || x >= nx || y >= ny)
            return 0;
        return ch[static_cast<std::size_t>(y) * nx + x];
    }
    bool blank(int x, int y) const { return at(x, y) == 0; }
    std::string rowText(int y) const {
        std::string text;
        for (int x = 0; x < nx; ++x) {
            const uint32_t cp = at(x, y);
            if (cp == 0)
                text += ' ';
            else if (cp < 128)
                text += static_cast<char>(cp);
            else
                text += '#'; // a wide/graphic glyph: present, not spelled out
        }
        return text;
    }
    bool contains(const std::string& needle) const {
        for (int y = 0; y < ny; ++y) {
            if (rowText(y).find(needle) != std::string::npos)
                return true;
        }
        return false;
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

int paintedOutside(const Grid& grid, const ui::Rect& r) {
    int count = 0;
    for (int y = 0; y < grid.ny; ++y)
        for (int x = 0; x < grid.nx; ++x) {
            const bool inside = x >= static_cast<int>(r.x) && x < static_cast<int>(r.right()) &&
                                y >= static_cast<int>(r.y) && y < static_cast<int>(r.bottom());
            if (!inside && !grid.blank(x, y))
                ++count;
        }
    return count;
}

int paintedInside(const Grid& grid, const ui::Rect& r) {
    int count = 0;
    for (int y = static_cast<int>(r.y); y < static_cast<int>(r.bottom()); ++y)
        for (int x = static_cast<int>(r.x); x < static_cast<int>(r.right()); ++x)
            count += grid.blank(x, y) ? 0 : 1;
    return count;
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

Grid frame(const std::function<void(ImDrawList*, const ui::Metrics&)>& body,
           const std::function<void(ImGuiIO&)>& inject = nullptr) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(kScreenW), static_cast<float>(kScreenH));
    io.DeltaTime = 1.0f / 60.0f;
    if (inject)
        inject(io);
    ImTui_ImplText_NewFrame();
    ImGui::NewFrame();
    const ui::Metrics m = gridMetrics();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(kScreenW), static_cast<float>(kScreenH)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("toast-frame", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus);
    body(ImGui::GetWindowDrawList(), m);
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::Render();
    ImTui_ImplText_RenderDrawData(ImGui::GetDrawData(), &g_screen);
    return capture();
}

/// The vertex bounding box of one draw, for the pixel profile: a vertex is a pixel's worth of
/// drawing, so "the card paints inside its rect" is checkable with no display involved.
struct VertexBox {
    bool any = false;
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    int count = 0;
};

VertexBox vertexFrame(const std::function<void(ImDrawList*, const ui::Metrics&)>& body,
                      const ui::Metrics& m, const std::function<void(ImGuiIO&)>& inject = nullptr) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(4000.0f, 4000.0f);
    io.DeltaTime = 1.0f / 60.0f;
    if (inject)
        inject(io);
    ImTui_ImplText_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(4000.0f, 4000.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("toast-vertices", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int before = dl->VtxBuffer.Size;
    body(dl, m);
    VertexBox box;
    box.count = dl->VtxBuffer.Size - before;
    for (int i = before; i < dl->VtxBuffer.Size; ++i) {
        const ImVec2& pos = dl->VtxBuffer[i].pos;
        if (!box.any) {
            box.any = true;
            box.x0 = box.x1 = pos.x;
            box.y0 = box.y1 = pos.y;
        } else {
            box.x0 = std::min(box.x0, pos.x);
            box.y0 = std::min(box.y0, pos.y);
            box.x1 = std::max(box.x1, pos.x);
            box.y1 = std::max(box.y1, pos.y);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::Render();
    return box;
}

/// A frame wide enough for the toast stack, drawn in the **pixel profile**: the cards open their
/// own windows, so the body needs nothing but a frame to run in (the app's root window is what the
/// cards cover, and `stack-root` is that).
void stackFrame(const std::function<void()>& body,
                const std::function<void(ImGuiIO&)>& inject = nullptr, int w = 1400, int h = 800) {
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
    ImGui::Begin("stack-root", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground);
    body();
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::Render();
    ImTui_ImplText_RenderDrawData(ImGui::GetDrawData(), &g_screen);
}

// ---------------------------------------------------------------------------
// 1 + 2. The queue
// ---------------------------------------------------------------------------

model::Toast item(model::ToastKind kind, std::string title, std::string detail = {}) {
    model::Toast toast;
    toast.kind = kind;
    toast.title = std::move(title);
    toast.detail = std::move(detail);
    return toast;
}

constexpr std::int64_t kNow = 100000;

void checkLifetimes() {
    using model::ToastKind;
    check(model::toastLifetimeMs(ToastKind::Info, /*textGrid=*/false) == 4000,
          "pixel profile: info lives 4 s (got " +
              std::to_string(model::toastLifetimeMs(ToastKind::Info, false)) + ")");
    check(model::toastLifetimeMs(ToastKind::Success, false) == 8000,
          "pixel profile: success lives 8 s (got " +
              std::to_string(model::toastLifetimeMs(ToastKind::Success, false)) + ")");
    check(model::toastLifetimeMs(ToastKind::Warn, false) == 0, "pixel profile: warn persists");
    check(model::toastLifetimeMs(ToastKind::Error, false) == 0, "pixel profile: error persists");
    check(model::toastLifetimeMs(ToastKind::Info, true) == 2000, "grid: info flashes 2 s");
    check(model::toastLifetimeMs(ToastKind::Success, true) == 2000, "grid: success flashes 2 s");
    check(model::toastLifetimeMs(ToastKind::Warn, true) == 0, "grid: warn persists");
    check(model::toastLifetimeMs(ToastKind::Error, true) == 0, "grid: error persists");
    check(model::toastKindPersists(ToastKind::Warn) && model::toastKindPersists(ToastKind::Error),
          "the two persistent kinds are the warning and the error");
    check(!model::toastKindPersists(ToastKind::Info) &&
              !model::toastKindPersists(ToastKind::Success),
          "info and success are the two that expire");
    check(model::toastCapacity(false) == 3 && model::toastCapacity(true) == 1,
          "the depth is 3 in the pixel profile and 1 on the grid");
}

void checkQueue() {
    using model::ToastKind;
    model::UiState ui;

    model::raiseToast(ui, item(ToastKind::Info, "Copied ANSWER"), kNow, /*textGrid=*/false);
    check(ui.toasts.size() == 1, "one raise is one item");
    check(ui.toasts.back().expiresAtMs == kNow + 4000,
          "an info item is stamped from the injected clock (got " +
              std::to_string(ui.toasts.back().expiresAtMs) + ")");

    // A repeat refreshes instead of stacking: two presses of one control are one answer.
    model::raiseToast(ui, item(ToastKind::Info, "Copied ANSWER"), kNow + 1000, false);
    check(ui.toasts.size() == 1, "the same message twice is one item");
    check(ui.toasts.back().expiresAtMs == kNow + 1000 + 4000,
          "and the repeat refreshes its lifetime");

    // A different message is a second item, and the newest is the one a presentation shows.
    model::raiseToast(ui, item(ToastKind::Success, "Saved to /tmp/log.txt"), kNow, false);
    check(ui.toasts.size() == 2, "a different message is a second item");
    check(model::topToast(ui) != nullptr && model::topToast(ui)->title == "Saved to /tmp/log.txt",
          "`topToast` is the newest item");

    // Depth: the fourth item pushes the oldest transient out.
    model::raiseToast(ui, item(ToastKind::Info, "third"), kNow, false);
    model::raiseToast(ui, item(ToastKind::Info, "fourth"), kNow, false);
    check(ui.toasts.size() == 3,
          "the pixel profile keeps three (got " + std::to_string(ui.toasts.size()) + ")");
    check(ui.toasts.front().title != "Copied ANSWER", "the oldest item made room");
    check(ui.toasts.back().title == "fourth", "the newest item is the last one");

    // A warning outranks an info that arrives after it: the warning is never pushed out.
    model::UiState warned;
    model::raiseToast(warned, item(ToastKind::Warn, "ui.conf is read-only"), kNow, false);
    for (int i = 0; i < 4; ++i)
        model::raiseToast(warned, item(ToastKind::Info, "info " + std::to_string(i)), kNow, false);
    bool warningSurvived = false;
    for (const model::Toast& toast : warned.toasts)
        warningSurvived = warningSurvived || toast.title == "ui.conf is read-only";
    check(warningSurvived, "a transient never evicts an unacknowledged warning");

    // The grid holds one line: the incoming transient is dropped rather than shown in a queue
    // nobody can see, and a warning raised over a warning takes the row (both persist).
    model::UiState grid;
    model::raiseToast(grid, item(ToastKind::Error, "Save failed: EACCES"), kNow, true);
    model::raiseToast(grid, item(ToastKind::Success, "Saved to /tmp/log.txt"), kNow, true);
    check(grid.toasts.size() == 1 && grid.toasts.back().kind == ToastKind::Error,
          "the grid keeps the unanswered error instead of a newer success");
    model::raiseToast(grid, item(ToastKind::Warn, "late warning"), kNow, true);
    check(grid.toasts.size() == 1 && grid.toasts.back().title == "late warning",
          "a newer persistent item does take the one grid line");
    check(grid.toasts.back().expiresAtMs == 0, "a persistent item carries no expiry");

    // Expiry: the injected clock, not a sleep.
    model::UiState timed;
    model::raiseToast(timed, item(ToastKind::Info, "info"), 1000, false);
    model::expireToasts(timed, 4999);
    check(timed.toasts.size() == 1, "an item stays up before its lifetime is over");
    model::expireToasts(timed, 5000);
    check(timed.toasts.empty(), "and is dropped exactly at the end of it");
    model::raiseToast(timed, item(ToastKind::Warn, "warn"), 1000, false);
    model::expireToasts(timed, 1000 + 24 * 60 * 60 * 1000);
    check(timed.toasts.size() == 1, "no clock reading retires a persistent item");

    // Dismissal: the key answers the **item the presentations name** (the newest), a card's control
    // answers its own index, and a stale index changes nothing. The distinction is the review's
    // finding 1: popping the oldest left the item the row was advertising up - and on the grid with
    // one item the two readings happen to coincide, which is exactly why it shipped.
    model::UiState queue;
    model::raiseToast(queue, item(ToastKind::Error, "the warning"), 0, false);
    model::raiseToast(queue, item(ToastKind::Success, "the copy"), 0, false);
    check(model::topToast(queue) != nullptr && model::topToast(queue)->title == "the copy",
          "the newest item is the one a presentation names");
    check(model::dismissShownToast(queue) && queue.toasts.size() == 1 &&
              queue.toasts.back().title == "the warning",
          "`dismissShownToast` answers the newest item, not the oldest (the warning survives)");
    check(!model::dismissToastAt(queue, 5), "an out-of-range index changes nothing");
    check(model::dismissToastAt(queue, 0) && queue.toasts.empty(),
          "dismissing by index takes exactly that item");
    check(!model::dismissShownToast(queue), "an empty queue has nothing to dismiss");
    check(model::topToast(queue) == nullptr, "and no newest item");

    // Which profile says what: the grid's row is its presentation, the pixel profile's is the stack
    // - one item is never shown twice (review finding 2).
    model::UiState both;
    model::raiseToast(both, item(ToastKind::Success, "Copied ANSWER"), 0, false);
    check(views::nowBarFlashItem(both, /*textGrid=*/true) != nullptr &&
              views::nowBarFlashItem(both, true)->title == "Copied ANSWER",
          "the grid's now bar flashes the newest item");
    check(views::nowBarFlashItem(both, /*textGrid=*/false) == nullptr,
          "the pixel profile's row shows no flash: its presentation is the stack");
    check(model::topToast(both) != nullptr, "and the item is still up for the stack to draw");
}

// ---------------------------------------------------------------------------
// 3. The measure
// ---------------------------------------------------------------------------

void checkMeasure() {
    const ui::Metrics grid = gridMetrics();
    const ui::Metrics gui = guiMetrics();
    ui::ToastSpec spec;
    spec.title = "Copied ANSWER";
    spec.mark = ui::Icon::Ok;
    spec.rule = ui::ColorToken::stateOk;

    const ui::Size one = ui::measureToast(grid, spec);
    check(one.w > 0.0f && one.h == 2.0f * grid.pad2 + grid.row,
          "a one-line toast is pad2 + one row + pad2 on the grid (got " + sizeText(one) + ")");
    spec.detail = "the whole answer";
    const ui::Size two = ui::measureToast(grid, spec);
    check(two.h == one.h + grid.row, "a detail adds one row (got " + sizeText(two) + ")");
    check(two.w >= one.w, "and never narrows the card");
    spec.action = "Dismiss";
    const ui::Size three = ui::measureToast(grid, spec);
    check(three.h == two.h + grid.pad1 + grid.row,
          "an action adds pad1 and one row (got " + sizeText(three) + ")");

    ui::ToastSpec wider;
    wider.title = "Copied ANSWER with a much longer title than the first one";
    check(ui::measureToast(grid, wider).w > one.w, "a longer title grows the card");

    // The design's 360 px card: `want` is a floor, and the card is exactly it when the content
    // fits - 45 gu at 100 %.
    const float wantW = views::kToastWidthGu * gui.gu;
    const ui::Size guiCard = ui::measureToast(gui, spec, ui::Size{wantW, 0.0f});
    check(guiCard.w == wantW,
          "the pixel profile's card is the design's 360 px (got " + num(guiCard.w) + ")");
    check(wantW == 360.0f, "which is 45 gu at 100 % (got " + num(wantW) + ")");
    ui::ToastSpec tiny;
    tiny.title = "ok";
    check(ui::measureToast(gui, tiny).w < wantW, "a short title measures narrower than the card");
    check(ui::measureToast(gui, tiny, ui::Size{wantW, 0.0f}).w == wantW,
          "the caller's wish is a floor, never a cap");

    // Whole units in both profiles: a fractional card would break the cell grid.
    const ui::Metrics scaled = ui::metricsFor(false, 1.5f, 1.0f);
    for (const ui::Metrics& m : {grid, gui, scaled}) {
        const ui::Size s = ui::measureToast(m, spec);
        check(s.w == std::floor(s.w) && s.h == std::floor(s.h),
              "measureToast answers whole units (" + sizeText(s) + ")");
        check(s.w > 0.0f && s.h > 0.0f, "and a positive size");
    }
}

// ---------------------------------------------------------------------------
// 4 + 5. Mixed units, and render == size
// ---------------------------------------------------------------------------

void checkMixedUnits() {
    ui::ToastSpec spec;
    spec.title = "Copied ANSWER";
    spec.detail = "the whole answer";
    const Grid grid = frame([&](ImDrawList*, const ui::Metrics& m) {
        // The pixels' rect in a cell frame: legal-looking numbers, the wrong unit.
        ui::toast(spec, m, rectPixels(2.0f, 2.0f, 20.0f, 4.0f));
    });
    check(paintedInside(grid, rectCells(0.0f, 0.0f, 60.0f, 12.0f)) == 0,
          "a rect in the other profile's unit paints nothing on the grid");

    const ui::Metrics gui = guiMetrics();
    const VertexBox box = vertexFrame(
        [&](ImDrawList*, const ui::Metrics& m) {
            ui::toast(spec, m, rectCells(10.0f, 10.0f, 360.0f, 60.0f));
        },
        gui);
    check(box.count == 0, "and nothing in the vertex list either");
}

void checkRenderGrid() {
    ui::ToastSpec spec;
    spec.title = "Copied ANSWER";
    spec.detail = "the whole answer";
    spec.mark = ui::Icon::Ok;
    spec.rule = ui::ColorToken::stateOk;
    const ui::Metrics m = gridMetrics();
    const ui::Size size = ui::measureToast(m, spec);
    const ui::Rect r = rectCells(2.0f, 2.0f, size.w, size.h);
    const Grid grid = frame([&](ImDrawList*, const ui::Metrics&) { ui::toast(spec, m, r); });
    check(paintedOutside(grid, r) == 0, "a toast paints nothing outside its measured rect (" +
                                            std::to_string(paintedOutside(grid, r)) + " cell(s))");
    check(paintedInside(grid, r) > 0, "and paints the card inside it");
    check(grid.contains("Copied ANSWER"), "the title is drawn");
    check(grid.contains("the whole answer"), "the detail is drawn on the second row");
    const std::string firstRow = grid.rowText(static_cast<int>(r.y) + 2);
    check(firstRow.find("Copied ANSWER") != std::string::npos,
          "the title sits on the card's own row");

    // The action label and the dismiss cross share the card without overlapping.
    ui::ToastSpec actionable = spec;
    actionable.action = "Dismiss";
    const ui::Size tall = ui::measureToast(m, actionable);
    const ui::Rect box = rectCells(2.0f, 3.0f, tall.w, tall.h);
    const Grid withAction =
        frame([&](ImDrawList*, const ui::Metrics&) { ui::toast(actionable, m, box); });
    check(paintedOutside(withAction, box) == 0, "the action card paints inside its rect");
    check(withAction.contains("Dismiss"), "the action's label is drawn");
}

void checkRenderPixels() {
    // The pixel profile draws rects, not cells: the check is the vertex bounding box, the same
    // one `./test_primitives` uses.
    ui::ToastSpec spec;
    spec.title = "Saved to /tmp/raw.log";
    spec.detail = "128 records";
    spec.mark = ui::Icon::Ok;
    spec.rule = ui::ColorToken::stateOk;
    const ui::Metrics m = guiMetrics();
    const ui::Size size = ui::measureToast(m, spec, ui::Size{views::kToastWidthGu * m.gu, 0.0f});
    const ui::Rect r = rectPixels(40.0f, 60.0f, size.w, size.h);
    const VertexBox box =
        vertexFrame([&](ImDrawList*, const ui::Metrics&) { ui::toast(spec, m, r); }, m);
    check(box.any && box.count > 0, "the pixel card draws something");
    check(box.x0 >= r.x - 0.5f && box.x1 <= r.right() + 0.5f && box.y0 >= r.y - 0.5f &&
              box.y1 <= r.bottom() + 0.5f,
          "and every vertex lands inside the measured rect (" + num(box.x0) + "," + num(box.y0) +
              " - " + num(box.x1) + "," + num(box.y1) + ")");
}

// ---------------------------------------------------------------------------
// 6. The controls
// ---------------------------------------------------------------------------

void checkControls() {
    const ui::Metrics m = gridMetrics();
    ui::ToastSpec spec;
    spec.title = "Save failed: EACCES";
    spec.mark = ui::Icon::Error;
    spec.rule = ui::ColorToken::stateDanger;
    spec.action = "Dismiss";
    const ui::Size size = ui::measureToast(m, spec);
    const ui::Rect r = rectCells(1.0f, 1.0f, size.w, size.h);

    // The dismiss cross is the top-right square: `pad2` in from the card's edges.
    const float crossX = r.right() - m.pad2 - m.row * 0.5f;
    const float crossY = r.y + m.pad2 + m.row * 0.5f;
    bool dismissed = false;
    bool actioned = false;
    const auto draw = [&](const std::function<void(ImGuiIO&)>& inject) {
        frame(
            [&](ImDrawList*, const ui::Metrics&) {
                ui::toast(spec, m, r);
                dismissed = dismissed || ui::toastDismissPressed(m, r);
                actioned = actioned || ui::toastActionPressed(spec, m, r);
            },
            inject);
    };
    draw(nullptr);
    check(!dismissed && !actioned, "a card answers no press before one arrives");

    const auto clickAt = [&](float x, float y) {
        draw([x, y](ImGuiIO& io) { io.AddMousePosEvent(x, y); });
        draw([](ImGuiIO& io) { io.AddMouseButtonEvent(0, true); });
        draw([](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); });
    };

    clickAt(crossX, crossY);
    check(dismissed, "a click on the dismiss cross answers the dismiss");
    check(!actioned, "and not the action");

    dismissed = false;
    actioned = false;
    // The action sits on the card's last row, right-aligned `pad2` from the edge.
    const float actionW = ui::measureButton(m, ui::ButtonSpec{spec.action}).w;
    clickAt(r.right() - m.pad2 - actionW * 0.5f, r.bottom() - m.pad2 - m.row * 0.5f);
    check(actioned, "a click on the action answers the action");
    check(!dismissed, "and not the dismiss");

    // A card with no action: the query answers false even where the button would be, and a card
    // that was never drawn answers nothing at all.
    ui::ToastSpec plain = spec;
    plain.action = "";
    bool plainAction = true;
    frame(
        [&](ImDrawList*, const ui::Metrics&) {
            ui::toast(plain, m, r);
            plainAction = ui::toastActionPressed(plain, m, r);
        },
        [&](ImGuiIO& io) { io.AddMousePosEvent(r.right() - m.pad2 - 4.0f, r.bottom() - 2.0f); });
    check(!plainAction, "a card without an action answers nothing");
    bool staleDismiss = true;
    frame([&](ImDrawList*, const ui::Metrics&) {
        staleDismiss = ui::toastDismissPressed(m, rectCells(40.0f, 8.0f, 12.0f, 4.0f));
    });
    check(!staleDismiss, "a rect no card was drawn at answers nothing");
}

// ---------------------------------------------------------------------------
// 7. The stack
// ---------------------------------------------------------------------------

void checkStack() {
    const ui::Metrics m = guiMetrics();
    ui::Rect area = rectPixels(0.0f, 0.0f, 1280.0f, 760.0f);
    model::UiState ui;
    model::raiseToast(ui, item(model::ToastKind::Info, "Copied USER"), 0, false);
    model::raiseToast(ui, item(model::ToastKind::Success, "Saved to /tmp/raw.log"), 0, false);
    model::raiseToast(ui, item(model::ToastKind::Error, "Save failed: EACCES"), 0, false);

    const std::vector<ui::Rect> rects = views::toastStackRects(ui, area, m);
    check(rects.size() == 3,
          "three items make three cards (got " + std::to_string(rects.size()) + ")");
    if (rects.size() == 3) {
        check(rects[0].y > rects[1].y && rects[1].y > rects[2].y,
              "the newest card is the lowest one");
        for (const ui::Rect& rect : rects) {
            check(rect.right() == area.right() - 0.0f,
                  "every card is right-aligned to the area (" + num(rect.right()) + ")");
            check(rect.w == views::kToastWidthGu * m.gu,
                  "every card is the design's width (" + num(rect.w) + ")");
            check(rect.y >= area.y && rect.bottom() <= area.bottom(),
                  "every card is inside the area it was given");
            check(!rect.cells, "the pixel profile's card is a pixel rect");
        }
        check(rects[1].bottom() == rects[0].y - m.pad1, "two cards are one `pad1` apart");
    }

    // A fourth item: the stack stays three deep (the queue already dropped the oldest).
    model::raiseToast(ui, item(model::ToastKind::Info, "fourth"), 0, false);
    check(ui.toasts.size() == 3, "the queue keeps three");
    check(views::toastStackRects(ui, area, m).size() == 3, "and the stack draws three");

    // An area with no room for a card draws none - never half a card.
    check(views::toastStackRects(ui, rectPixels(0.0f, 0.0f, 1280.0f, 10.0f), m).empty(),
          "an area too short for one card holds no cards");

    // A card wider than the area (a long path, D-GEOM's 640 px floor) is clamped to the area
    // instead of sliding out of the display; `ui::toast()` then clips its strings (review finding
    // 3).
    model::UiState wide;
    model::raiseToast(wide,
                      item(model::ToastKind::Error,
                           "Save failed: /tmp/a/very/long/path/that/does/not/fit/anywhere.log"),
                      0, false);
    const ui::Rect narrowArea = rectPixels(0.0f, 0.0f, 300.0f, 600.0f);
    const std::vector<ui::Rect> clamped = views::toastStackRects(wide, narrowArea, m);
    check(clamped.size() == 1 && clamped[0].w == narrowArea.w && clamped[0].x == narrowArea.x,
          "a card wider than the area is clamped to it, never pushed out of the display");
    check(views::kToastWidthGu * m.gu > narrowArea.w,
          "premise: the design's card (45 gu) is wider than that area, so the clamp is what is "
          "being measured");

    // The grid's presentation is the now bar's flash: the stack draws nothing there.
    check(views::toastStackRects(ui, rectCells(0.0f, 0.0f, 100.0f, 30.0f), gridMetrics()).empty(),
          "the text grid has no stack (its flash is the now bar's)");
    // `renderToastStack` must be harmless on the grid even with items up.
    views::renderToastStack(ui, rectCells(0.0f, 0.0f, 100.0f, 30.0f), gridMetrics());
    check(ui.toasts.size() == 3, "and drawing it there drops nothing");
}

// ---------------------------------------------------------------------------
// 9. The stack's controls
// ---------------------------------------------------------------------------

void checkStackControls() {
    const ui::Metrics m = guiMetrics();
    const ui::Rect area = rectPixels(0.0f, 0.0f, 1400.0f, 700.0f);
    model::UiState ui;
    model::raiseToast(ui, item(model::ToastKind::Info, "oldest"), 0, false);
    model::raiseToast(ui, item(model::ToastKind::Success, "middle"), 0, false);
    model::raiseToast(ui, item(model::ToastKind::Error, "newest"), 0, false);
    std::vector<ui::Rect> rects = views::toastStackRects(ui, area, m);
    check(rects.size() == 3, "the interaction fixture has three cards");

    // The dismiss cross: one row square, `pad2` in from the card's top-right corner.
    const auto crossOf = [&m](const ui::Rect& card) {
        return ImVec2(card.right() - m.pad2 - m.row * 0.5f, card.y + m.pad2 + m.row * 0.5f);
    };
    // The one action: the `Dismiss` button on the card's last row, `pad2` in from the right edge.
    const float actionW = ui::measureButton(m, ui::ButtonSpec{"Dismiss"}).w;
    const auto actionOf = [&m, actionW](const ui::Rect& card) {
        return ImVec2(card.right() - m.pad2 - actionW * 0.5f,
                      card.bottom() - m.pad2 - m.row * 0.5f);
    };
    const auto drawStack = [&](const std::function<void(ImGuiIO&)>& inject) {
        stackFrame([&]() { views::renderToastStack(ui, area, m); }, inject);
    };
    /// The three-frame click `checkControls()` uses: the position lands on the next frame's input,
    /// the press on the one after it (and the press is what a card answers).
    const auto clickAt = [&](float x, float y) {
        drawStack([x, y](ImGuiIO& io) { io.AddMousePosEvent(x, y); });
        drawStack([](ImGuiIO& io) { io.AddMouseButtonEvent(0, true); });
        drawStack([](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); });
    };

    drawStack(nullptr);
    check(ui.toasts.size() == 3, "a frame with no press drops nothing");

    // The *upper* card is `[1]` in the drawn order and `toasts.size() - 1 - 1` in the queue: the
    // index mapping, and only that item - a press is one answer.
    const ImVec2 upperCross = crossOf(rects[1]);
    clickAt(upperCross.x, upperCross.y);
    check(ui.toasts.size() == 2 && ui.toasts.back().title == "newest" &&
              ui.toasts.front().title == "oldest",
          "a click on the middle card's cross drops the middle item only (got " +
              std::to_string(ui.toasts.size()) + " left)");

    // The bottom card is the newest, and its `Dismiss` action is the second control: the same
    // answer through a different rect (the persistent card's one action).
    rects = views::toastStackRects(ui, area, m);
    check(rects.size() == 2, "two items make two cards");
    const ImVec2 newestAction = actionOf(rects[0]);
    clickAt(newestAction.x, newestAction.y);
    check(ui.toasts.size() == 1 && ui.toasts.back().title == "oldest",
          "a click on the newest card's `Dismiss` action drops that card (got " +
              std::to_string(ui.toasts.size()) + " left)");

    // A click outside every card answers nothing, and the last card still answers its own cross.
    clickAt(50.0f, 50.0f);
    check(ui.toasts.size() == 1, "a click away from the stack drops nothing");
    const ImVec2 lastCross = crossOf(views::toastStackRects(ui, area, m).at(0));
    clickAt(lastCross.x, lastCross.y);
    check(ui.toasts.empty(), "and the last card's cross empties the queue");
}

// ---------------------------------------------------------------------------
// 8. The look
// ---------------------------------------------------------------------------

void checkLook() {
    using model::ToastKind;
    check(views::feedbackLook(ToastKind::Info).ink == ui::ColorToken::textMid,
          "info is the mid ink with the info mark");
    check(views::feedbackLook(ToastKind::Info).mark == ui::Icon::Info, "info draws `Info`");
    check(views::feedbackLook(ToastKind::Success).mark == ui::Icon::Ok &&
              views::feedbackLook(ToastKind::Success).ink == ui::ColorToken::stateOk,
          "success draws the ok mark in the ok ink");
    check(views::feedbackLook(ToastKind::Warn).mark == ui::Icon::Warn &&
              views::feedbackLook(ToastKind::Warn).ink == ui::ColorToken::stateWarn,
          "a warning draws the warn mark in the warn ink");
    check(views::feedbackLook(ToastKind::Error).mark == ui::Icon::Error &&
              views::feedbackLook(ToastKind::Error).ink == ui::ColorToken::stateDanger,
          "an error draws the error mark in the danger ink");

    // The card's spec: the severity owns the mark and the rule, the words stay readable, and only
    // a persistent item offers the one action.
    const ui::ToastSpec info = views::toastSpecFor(item(ToastKind::Info, "Copied ANSWER"));
    check(info.action[0] == '\0', "a transient card carries no action");
    const ui::ToastSpec error = views::toastSpecFor(item(ToastKind::Error, "Save failed"));
    check(std::strcmp(error.action, "Dismiss") == 0, "a persistent card offers `Dismiss`");
    check(error.titleInk == ui::ColorToken::textHi && error.detailInk == ui::ColorToken::textMid,
          "a card's words keep the readable inks");
    check(error.rule == ui::ColorToken::stateDanger, "and its rule is the severity's");
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0)
            g_verbose = true;
        else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0)
            usage(argv[0], "print every check with -v");
        else
            usage(argv[0], "unknown argument");
    }

    // One glyph column for every machine that runs this: `LLMFUN_ASCII=0` outranks TERM and the
    // locale in the detection order.
    ::setenv("LLMFUN_ASCII", "0", 1);

    checkLifetimes();
    checkQueue();
    checkLook();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImTui_ImplText_Init();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(kScreenW), static_cast<float>(kScreenH));
    io.DeltaTime = 1.0f / 60.0f;

    // One warm-up frame: the pixel-profile measures need a live font, and the text backend builds
    // its atlas on the first one.
    frame([](ImDrawList*, const ui::Metrics&) {});

    checkMeasure();
    checkMixedUnits();
    checkRenderGrid();
    checkRenderPixels();
    checkControls();
    checkStack();
    checkStackControls();

    ImTui_ImplText_Shutdown();
    ImGui::DestroyContext();

    std::printf("%s: %d check(s), %d failure(s)\n", g_failures == 0 ? "PASS" : "FAIL", g_checks,
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
