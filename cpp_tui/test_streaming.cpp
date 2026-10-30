// test_streaming.cpp
//
// Streaming v2 (task 38, design FR-08 / §4.5): the live answer made legible and cheap.
//
// What this binary proves, in order:
//
//   1. the sources   the streaming state is the frame's (`refreshStreamClock` is called once per
//                    frame in `views/shell.cpp`), the caret is the markdown wrapper's
//                    (`ui::withStreamingCaret`, the one place that knows what a fence is), no
//                    view names a v4 type, a profile flag or a display size, and the bounded
//                    window counts the *stored* groups (`storedGroupCount`).
//   2. the clock     a fake clock drives every number: the answer's elapsed time, the reasoning's
//                    duration (0 while it is still reasoning, frozen when the answer begins) and
//                    the caret's blink phase, which changes twice a second and never once per
//                    frame - 30 reads of one half-period answer the same value.
//   3. the throttle  the store accepts one snapshot per window (~30 fps), parks the newest inside
//                    it, and treats a byte-identical resend as no repaint at all; a stop applies
//                    a parked snapshot before the clock freezes, so the partial answer is never
//                    one window short.
//   4. the identity  across 300 updates of a markdown-heavy answer the stream group's id and its
//                    two blocks (`{group, kStreamMessageId, Thinking}` and `... Assistant`) never
//                    move, and the group's `revision` advances exactly when the text does.
//   5. the caret     drawn only while the answer is arriving: on the body's last line, absent in
//                    the blink's off half (which holds the same cells, so the *drawn* last row
//                    does not move), absent for a cancelled or finished answer, and never inside a
//                    fence delimiter, a table row or a rule line.
//   6. the header    `AGENT · writing · 00:12`, still `thinking` (with the run's clock) before the
//                    first character, `cancelled · 00:12` after a stop, and `Thinking · 3.2 s`
//                    once the reasoning has ended.
//   7. the height    within one revision the group's height never shrinks (the caret's blink and
//                    a disclosure toggled at the same text are the two cases), so a live answer
//                    cannot oscillate.
//   8. the window    the live answer is drawn after the bounded window, not inside it: three
//                    stored groups plus the stream is four drawn groups and the paging count is
//                    the stored conversation's.
//   9. the cost      one streaming frame stays inside NFR-02's text-grid budget (8 ms).
//  10. the goldens   the live answer with a caret, the cancelled answer, a reasoning-only
//                    stream, and a stream whose last line is a table row (the caret gets its
//                    own line there, so the table keeps being a table), as text+inks screens.
//
// The grid is the real text backend (the `TScreen` pattern of `test_session_filter_smoke.cpp`)
// with the real text-mode markdown profile, so a golden is what the terminal would show.
//
// Exit codes: 0 = all checks pass; 1 = a check failed; 2 = usage error.
//
//   ./test_streaming [-v] [--update] [--dump]
//
// `--update` rewrites the goldens; do that only for a deliberate rendering change.

#include "imtui/imtui.h" // imgui + ImTui_ImplText_*

#include "tui.h" // llmfun::tui: TuiState, tuiRender, the markdown text-mode lifecycle

#include "model/block.h"
#include "model/status_chips.h"
#include "model/stream_state.h"
#include "model/ui_state.h"
#include "store/thread_store.h"
#include "ui/icons.h"
#include "ui/layout.h"
#include "ui/markdown.h"
#include "ui/metrics.h"
#include "ui/table.h"
#include "ui/tokens.h"
#include "views/blocks.h"
#include "views/thread.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <chrono>
#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace tui = llmfun::tui;
namespace ui = llmfun::tui::ui;
namespace model = llmfun::tui::model;
namespace store = llmfun::tui::store;
namespace views = llmfun::tui::views;

namespace {

int g_checks = 0;
int g_failures = 0;
bool g_verbose = false;
bool g_update = false;
bool g_dump = false;

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

void phase(const char* name) {
    if (g_verbose)
        std::printf("--- %s\n", name);
}

[[noreturn]] void usage(const char* argv0, const char* problem) {
    std::fprintf(stderr, "usage: %s [-v] [--update] [--dump]   (%s)\n", argv0, problem);
    std::exit(2);
}

std::string toString(long long value) { return std::to_string(value); }

// ---------------------------------------------------------------------------
// The cell grid the text backend writes
// ---------------------------------------------------------------------------

ImTui::TScreen g_screen;

constexpr int kScreenW = 100;
constexpr int kScreenH = 30;

/// The fake clock every clock check uses: a whole number of blink half-periods, so which half a
/// reading falls in is stated in the check instead of depending on the wall clock.
constexpr long long kT0 = 1000000;

/// How much of the answer's clock one fixture update advances (the stream test's cadence).
constexpr long long kStreamStepMs = 4000;

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
    bool untouched(int x, int y) const {
        return at(x, y) == 0 && bg[static_cast<std::size_t>(y) * nx + x] == 0;
    }

    std::string rowText(int y) const {
        std::string text;
        for (int x = 0; x < nx; ++x) {
            const uint32_t cp = at(x, y);
            text += cp == 0 ? std::string(" ") : utf8Of(cp);
        }
        return text;
    }

    bool rowContains(int y, const std::string& needle) const {
        return rowText(y).find(needle) != std::string::npos;
    }
    bool contains(const std::string& needle) const {
        for (int y = 0; y < ny; ++y)
            if (rowContains(y, needle))
                return true;
        return false;
    }
    int countCell(uint32_t cp) const {
        int count = 0;
        for (uint32_t cell : ch)
            count += cell == cp ? 1 : 0;
        return count;
    }
    /// The last row that holds anything (the row a "still writing" answer ends on).
    int lastInkRow() const {
        for (int y = ny - 1; y >= 0; --y)
            for (int x = 0; x < nx; ++x)
                if (at(x, y) != 0)
                    return y;
        return -1;
    }

    std::string dump() const {
        std::string text;
        for (int y = 0; y < ny; ++y) {
            std::string row;
            for (int x = 0; x < nx; ++x) {
                const uint32_t cp = at(x, y);
                row += (cp >= 32 && cp < 127) ? std::string(1, static_cast<char>(cp))
                                              : (cp == 0 ? std::string(" ") : std::string("."));
            }
            while (!row.empty() && row.back() == ' ')
                row.pop_back();
            text += row + "\n";
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
    grid.fg.resize(cells);
    grid.bg.resize(cells);
    for (std::size_t i = 0; i < cells; ++i) {
        grid.ch[i] = g_screen.data[i].ch;
        grid.fg[i] = g_screen.data[i].fg;
        grid.bg[i] = g_screen.data[i].bg;
    }
    return grid;
}

/// One frame of the text grid: a decoration-free root window filling the display, the body
/// drawing inside it, then the frame rendered through the real backend.
Grid frame(const std::function<void(const ui::Metrics&)>& body) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(kScreenW), static_cast<float>(kScreenH));
    io.DeltaTime = 1.0f / 30.0f; // the streaming cadence, so the frame never pretends to be 60
    ImTui_ImplText_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(kScreenW), static_cast<float>(kScreenH)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("streaming-frame", nullptr,
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

ui::Rect cellRect(float x, float y, float w, float h) {
    ui::Rect r;
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    r.cells = true;
    return r;
}

/// One message of the v4 store, in the shape D writes it (`"[<nr>] <body> [<time>]"` for a
/// stored line, nothing for the streamed one).
tui::ChatMessage message(size_t id, tui::ChatMessageType type, const std::string& summary,
                         const std::string& text, const std::string& thinking = {}) {
    tui::ChatMessage msg;
    msg.id = id;
    msg.type = type;
    msg.summary = summary;
    msg.text = text;
    msg.thinking = thinking;
    return msg;
}

// ---------------------------------------------------------------------------
// Harness init/shutdown: the real app's order (markdown text mode BEFORE the backend init,
// which locks the font atlas), then the theme.
// ---------------------------------------------------------------------------

void harnessInit(tui::TuiState& state) {
    std::setlocale(LC_ALL, "");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    tui::initMarkdownTextMode(state);
    ImTui_ImplText_Init();
    tui::applyUiTheme();
    tui::finishMarkdownTextMode(state);
    ImGui::GetIO().DisplaySize = ImVec2(static_cast<float>(kScreenW), static_cast<float>(kScreenH));
}

void harnessShutdown(tui::TuiState& state) {
    tui::destroyMarkdown(state);
    ImTui_ImplText_Shutdown();
    ImGui::DestroyContext();
}

/// The markdown-heavy answer the stream tests grow one prefix at a time. It is a *file* so the
/// fixture is the document a user would see, not a string buried in the harness.
std::string fixtureDocument() {
    std::ifstream in(std::string(TEST_STREAMING_FIXTURE));
    if (!in.good()) {
        std::fprintf(stderr, "FAIL: the streaming fixture is readable (%s)\n",
                     TEST_STREAMING_FIXTURE);
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------------------
// The fixture: a workbench state with a thread, drawn into the 100x30 grid
// ---------------------------------------------------------------------------

struct Fixture {
    tui::TuiState state;

    Fixture() {
        state.readyStatus = false; // a run is in flight: the composer's Stop is up
        state.ui.view = model::ThreadView::Ready;
        state.threadAdapter.bind(state);
    }

    void addStored(const tui::ChatMessage& msg) {
        state.chat.outputLines.push_back(msg);
        state.chat.hasStreamMsg = false;
    }

    /// Put a streamed answer in the store and derive its clock at `nowMs` - the same two writes
    /// the store and the frame layer make, with the clock stated instead of read.
    void stream(const std::string& text, const std::string& thinking, long long nowMs,
                bool cancelRequested = false) {
        state.chat.streamMsg = message(0, tui::ChatMessageType::Assistant, "", text, thinking);
        state.chat.hasStreamMsg = true;
        model::refreshStreamClock(state.ui.thread.stream, /*hasStream=*/true,
                                  /*hasText=*/!text.empty(),
                                  /*hasThinking=*/!thinking.empty(), nowMs, cancelRequested);
    }

    /// Advance the clock without touching the text (a frame that changed nothing).
    void tick(long long nowMs, bool cancelRequested = false) {
        model::refreshStreamClock(state.ui.thread.stream, state.chat.hasStreamMsg,
                                  !state.chat.streamMsg.text.empty(),
                                  !state.chat.streamMsg.thinking.empty(), nowMs, cancelRequested);
    }

    size_t streamGroupIndex() const { return state.threadAdapter.storedGroupCount(); }
    store::GroupView streamGroup() const { return state.threadAdapter.groupAt(streamGroupIndex()); }

    /// Draw the thread into the 100x30 frame and answer the captured grid.
    Grid draw(int windowGroups = 40) {
        state.threadAdapter.bind(state);
        const ui::Rect region = cellRect(0.0f, 0.0f, float(kScreenW), float(kScreenH) - 1.0f);
        const ui::Rect earlier = cellRect(0.0f, 0.0f, 0.0f, 0.0f);
        return frame([&](const ui::Metrics& m) {
            views::renderThread(state.ui, state.threadAdapter, region, earlier, m, windowGroups,
                                /*compact=*/false, /*acceptKeys=*/true);
        });
    }
};

/// The header `views::blockHeaderText()` prints for one block of the stream group.
std::string headerOf(Fixture& fixture, std::size_t block) {
    const model::BlockDesc desc =
        fixture.state.threadAdapter.blockAt(fixture.streamGroupIndex(), block);
    return views::blockHeaderText(desc, views::BlockDraw{desc.expanded, false}, /*textGrid=*/true);
}

// ---------------------------------------------------------------------------
// 1. The sources obey the layering rules
// ---------------------------------------------------------------------------

bool readWholeFile(const std::string& path, std::string& out) {
    std::ifstream in(path);
    if (!in.good())
        return false;
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return true;
}

void checkSource() {
    const std::string dir = TEST_STREAMING_SOURCE_DIR;
    struct Row {
        const char* file;
        bool views;
    };
    const Row rows[] = {{"views/thread.cpp", true},
                        {"views/blocks.cpp", true},
                        {"views/nowbar.cpp", true},
                        {"store/thread_store.cpp", false},
                        {"ui/markdown.cpp", false}};
    for (const Row& row : rows) {
        std::string source;
        const std::string path = dir + "/" + row.file;
        check(readWholeFile(path, source), std::string("readable: ") + row.file);
        if (source.empty())
            continue;
        if (row.views) {
            check(source.find("ChatMessage") == std::string::npos,
                  std::string(row.file) + " names no v4 type (D-ADAPTER)");
            check(source.find("RenderGroup") == std::string::npos,
                  std::string(row.file) + " names no v4 grouping type");
            check(source.find("tuiIsTextGrid") == std::string::npos,
                  std::string(row.file) + " never asks which profile is up");
            check(source.find("ImVec4(") == std::string::npos,
                  std::string(row.file) + " has no hand-made colour");
            check(source.find("DisplaySize") == std::string::npos,
                  std::string(row.file) + " reads no display size");
            check(source.find("#include \"tui.h\"") == std::string::npos,
                  std::string(row.file) + " does not include tui.h");
        }
    }
    std::string thread;
    if (readWholeFile(dir + "/views/thread.cpp", thread)) {
        check(thread.find("storedGroupCount") != std::string::npos,
              "the thread's window counts the stored groups (the live answer is outside it)");
        check(thread.find("hasStream") != std::string::npos,
              "and appends the live answer after the window");
    }
    std::string shell;
    if (readWholeFile(dir + "/views/shell.cpp", shell))
        check(shell.find("refreshStreamClock") != std::string::npos,
              "the frame layer derives the live answer's clock once per frame");
    std::string markdown;
    if (readWholeFile(dir + "/ui/markdown.cpp", markdown))
        check(markdown.find("withStreamingCaret") != std::string::npos,
              "the caret is appended by the markdown wrapper (the one place that knows a fence)");
}

// ---------------------------------------------------------------------------
// 2. The clock: elapsed, the reasoning's duration and the blink - with a fake clock
// ---------------------------------------------------------------------------

void checkClock() {
    phase("clock");
    model::StreamState s;
    check(!s.active(), "a fresh clock is inactive");

    model::refreshStreamClock(s, /*hasStream=*/false, false, false, kT0, false);
    check(!s.active() && s.elapsedMs == 0 && !s.caretOn, "no stream answers 00:00 and no caret");

    // The first sighting of a stream *is* its start.
    model::refreshStreamClock(s, /*hasStream=*/true, /*hasText=*/false, /*hasThinking=*/true, kT0,
                              false);
    check(s.startedMs == kT0, "the first sighting starts the clock");
    check(s.elapsedMs == 0, "the answer begins at 00:00");
    check(s.caretOn, "the caret's first half-period is on");
    check(s.thinkingElapsedMs == 0, "the reasoning's duration is 0 while it is still reasoning");

    // A blink is not a per-frame toggle: thirty reads inside one half-period answer the same.
    bool same = true;
    for (int i = 1; i <= 30; ++i) {
        model::refreshStreamClock(s, true, false, true, kT0 + i * 10, false);
        same = same && s.caretOn;
    }
    check(same, "30 frames inside one half-period keep the caret on (not a per-frame flip)");
    model::refreshStreamClock(s, true, false, true, kT0 + model::kStreamCaretHalfPeriodMs, false);
    check(!s.caretOn, "the next half-period turns it off");
    model::refreshStreamClock(s, true, false, true, kT0 + 2 * model::kStreamCaretHalfPeriodMs,
                              false);
    check(s.caretOn, "and the one after that turns it back on");

    // Elapsed is the injected clock's own arithmetic.
    model::refreshStreamClock(s, true, false, true, kT0 + 12000, false);
    check(s.elapsedMs == 12000,
          "elapsed is now - start (12000 ms, got " + toString(s.elapsedMs) + ")");

    // The answer's first character ends the reasoning, and the duration freezes there.
    model::refreshStreamClock(s, true, /*hasText=*/true, true, kT0 + 15200, false);
    check(s.sawText, "the answer's text has begun");
    check(s.thinkingEndedMs == kT0 + 15200, "the reasoning ended when the text began");
    check(s.thinkingElapsedMs == 15200,
          "and keeps the duration it took (15200 ms, got " + toString(s.thinkingElapsedMs) + ")");
    check(s.elapsedMs == 15200, "the answer's clock keeps running");
    model::refreshStreamClock(s, true, true, true, kT0 + 20000, false);
    check(s.thinkingElapsedMs == 15200, "a later frame does not extend the reasoning's duration");

    // A stop freezes the clock where the text stopped arriving and takes the caret away.
    model::refreshStreamClock(s, true, true, true, kT0 + 27000, /*cancelRequested=*/true);
    check(s.cancelled, "a cancel request marks the answer cancelled");
    check(s.elapsedMs == 27000,
          "the clock freezes at the stop (got " + toString(s.elapsedMs) + ")");
    check(!s.caretOn, "a cancelled answer has no caret");
    model::refreshStreamClock(s, true, true, true, kT0 + 40000, true);
    check(s.elapsedMs == 27000, "and stays frozen while the run winds down");

    // The phase word is the model's one vocabulary.
    check(std::string(model::streamPhaseWord(s, true)) == model::kStreamCancelledWord,
          "a stopped answer says `cancelled`");
    model::StreamState running;
    model::refreshStreamClock(running, true, false, false, kT0, false);
    check(std::string(model::streamPhaseWord(running, /*hasText=*/false)) ==
              model::phaseWord(model::Phase::Thinking),
          "a reasoning-only stream says `thinking`");
    check(std::string(model::streamPhaseWord(running, /*hasText=*/true)) ==
              model::phaseWord(model::Phase::Writing),
          "an answer with text says `writing`");

    // A stream that ends leaves nothing behind.
    model::StreamState ended = s;
    model::refreshStreamClock(ended, /*hasStream=*/false, false, false, kT0 + 50000, false);
    check(!ended.active() && ended.elapsedMs == 0 && !ended.cancelled && !ended.caretOn,
          "the stream's end clears the clock, the cancel mark and the caret");
}

// ---------------------------------------------------------------------------
// 3. The store's update throttle
// ---------------------------------------------------------------------------

void checkThrottle() {
    phase("throttle");
    Fixture fast;
    // 300 snapshots as fast as D's loop sends them (the real clock, no frame between): the first
    // is stored, the rest are coalesced inside one window, and the newest is parked - so nothing
    // is lost. The *invariants* are asserted unconditionally (every snapshot is accounted for,
    // and the parked one is the newest); the exact split is asserted only when the loop really
    // ran inside one window, because the window is the wall clock's and a loaded machine must not
    // turn the check into a lie.
    const auto burstStart = std::chrono::steady_clock::now();
    for (int i = 1; i <= 300; ++i)
        tui::tuiUpdateStreamChatMessage(fast.state, message(0, tui::ChatMessageType::Assistant, "",
                                                            "chunk " + std::to_string(i)));
    const double burstMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - burstStart)
            .count();
    const model::StreamState& stream = fast.state.ui.thread.stream;
    check(stream.applied + stream.coalesced + stream.unchanged == 300,
          "every snapshot is accounted for: stored, coalesced or unchanged (" +
              std::to_string(stream.applied) + " + " + std::to_string(stream.coalesced) + " + " +
              std::to_string(stream.unchanged) + ")");
    check(stream.applied >= 1, "the burst stored at least the snapshot it arrived on (got " +
                                   std::to_string(stream.applied) + ")");
    if (burstMs < static_cast<double>(model::kStreamUpdateIntervalMs)) {
        check(stream.applied == 1, "inside one window only the first snapshot is stored (got " +
                                       std::to_string(stream.applied) + ")");
        check(stream.coalesced == 299,
              "the other 299 are coalesced (got " + std::to_string(stream.coalesced) + ")");
    } else {
        check(stream.coalesced + stream.unchanged < 300,
              "the burst outran the window, so it was stored more than once");
    }
    check(fast.state.chat.pendingStream, "and the newest snapshot is parked");
    check(fast.state.chat.pendingStreamMsg.text == "chunk 300",
          "which is the newest, not a stale one (got '" + fast.state.chat.pendingStreamMsg.text +
              "')");

    // A resend of the newest text (what the store shows or is about to) is not a repaint.
    const std::uint64_t unchangedBefore = stream.unchanged;
    const std::uint64_t appliedBefore = stream.applied;
    const std::uint64_t coalescedBefore = stream.coalesced;
    tui::tuiUpdateStreamChatMessage(fast.state,
                                    message(0, tui::ChatMessageType::Assistant, "", "chunk 300"));
    check(stream.unchanged == unchangedBefore + 1, "a byte-identical resend costs no write");
    check(stream.applied == appliedBefore && stream.coalesced == coalescedBefore,
          "and changes neither counter");

    // A stop is the last word on the text: the parked snapshot is applied before the clock freezes.
    tui::tuiRequestCancel(fast.state);
    check(fast.state.chat.streamMsg.text == "chunk 300",
          "a stop applies the parked snapshot (got '" + fast.state.chat.streamMsg.text + "')");
    check(!fast.state.chat.pendingStream, "and clears the slot");
    check(stream.applied == appliedBefore + 1,
          "the stop's write is counted (got " + std::to_string(stream.applied) + ")");

    // Every snapshot lands when the window has elapsed: 300 updates over 300 windows.
    Fixture paced;
    for (int i = 1; i <= 300; ++i) {
        // "The window has elapsed": what the frame's flush sees 33 ms later.
        paced.state.ui.thread.stream.lastAppliedMs = 0;
        tui::tuiUpdateStreamChatMessage(
            paced.state, message(0, tui::ChatMessageType::Assistant, "", "t" + std::to_string(i)));
    }
    check(paced.state.ui.thread.stream.applied == 300,
          "one snapshot per window: all 300 are stored (got " +
              std::to_string(paced.state.ui.thread.stream.applied) + ")");
    check(paced.state.ui.thread.stream.coalesced == 0, "and none is parked");
    check(paced.state.chat.streamMsg.text == "t300", "the store ends on the newest text");
}

// ---------------------------------------------------------------------------
// 4 + 7. The identity across 300 updates, and the height within a revision
// ---------------------------------------------------------------------------

void checkIdentityAndHeight() {
    phase("identity");
    const std::string document = fixtureDocument();
    if (document.empty()) {
        check(false, "the streaming fixture is usable");
        return;
    }
    Fixture fixture;
    fixture.addStored(
        message(1, tui::ChatMessageType::User, "[1] stream this [0.1s]", "stream this"));
    // The reasoning arrives first (40 updates) and ends when the answer's first character does.
    constexpr int kReasoningUpdates = 40;

    bool identityStable = true;
    bool revisionMonotone = true;
    bool heightStableWithinRevision = true;
    std::uint64_t previousRevision = 0;
    size_t groupId = 0;
    long long now = kT0;
    long long reasoningEndedAt = 0;

    for (int step = 1; step <= 300; ++step) {
        const int textSteps = std::max(1, step - kReasoningUpdates);
        const std::size_t prefix =
            step <= kReasoningUpdates
                ? 0
                : std::max<std::size_t>(1, document.size() * static_cast<std::size_t>(textSteps) /
                                               300);
        const std::string text =
            step <= kReasoningUpdates ? std::string() : document.substr(0, prefix);
        const std::string reasoning = "First, read the document, then answer it in order.";
        // Four seconds of the answer's clock per update: the height samples below span two and a
        // half blinks, so they fit inside one step and the clock only ever moves forward - the
        // derivation treats a reading older than the last one as the same moment (a wall clock can
        // step back). One update per four seconds is slow for a stream and right for a clock test.
        now = kT0 + static_cast<long long>(step) * kStreamStepMs;
        if (step == 1)
            fixture.stream("", reasoning, now);
        else
            fixture.stream(text, reasoning, now);
        if (fixture.state.ui.thread.stream.thinkingEndedMs != 0 && reasoningEndedAt == 0)
            reasoningEndedAt = fixture.state.ui.thread.stream.thinkingEndedMs;

        const store::GroupView view = fixture.streamGroup();
        if (step == 1)
            groupId = view.id;
        identityStable = identityStable && view.id == groupId;
        // Both rows keep their identity: the reasoning (when there is one) and the body row.
        const size_t blocks = fixture.state.threadAdapter.blockCount(fixture.streamGroupIndex());
        const model::BlockDesc body =
            fixture.state.threadAdapter.blockAt(fixture.streamGroupIndex(), blocks - 1);
        identityStable =
            identityStable && body.id() == model::BlockId{groupId, model::kStreamMessageId,
                                                          model::BlockKind::Assistant};
        if (step > 1 && view.revision < previousRevision)
            revisionMonotone = false;
        previousRevision = view.revision;

        // The height within this revision. A markdown construct (a fenced code block, a bridged
        // table) needs a few frames to settle, so what must never happen is a *shrink* (the row
        // would move under the reader) and it must stop growing. Six samples 500 ms apart cover
        // two and a half blink periods, i.e. both halves of the caret's cycle twice.
        constexpr int kSamples = 6;
        float heights[kSamples] = {};
        for (int pass = 0; pass < kSamples; ++pass) {
            fixture.draw();
            heights[pass] = fixture.state.threadAdapter.measureHint(groupId);
            fixture.tick(now + static_cast<long long>(pass) * model::kStreamCaretHalfPeriodMs);
        }
        for (int pass = 1; pass < kSamples; ++pass) {
            if (heights[pass] < heights[pass - 1] - 0.01f) {
                heightStableWithinRevision = false;
                if (g_verbose)
                    std::fprintf(stderr, "     step %d shrank: %.1f -> %.1f\n", step,
                                 heights[pass - 1], heights[pass]);
            }
        }
        if (heights[kSamples - 1] != heights[kSamples - 2]) {
            heightStableWithinRevision = false;
            if (g_verbose)
                std::fprintf(stderr, "     step %d did not settle: %.1f -> %.1f\n", step,
                             heights[kSamples - 2], heights[kSamples - 1]);
        }
    }

    check(identityStable, "300 updates keep the stream group's id and both block ids stable");
    check(groupId != 0, "the stream group has an id (got " + std::to_string(groupId) + ")");
    check(revisionMonotone, "the group's revision never goes backwards");
    check(previousRevision >= 250, "and it advanced with the text (final revision " +
                                       std::to_string(previousRevision) + ")");
    check(heightStableWithinRevision,
          "within one revision the height never shrinks and settles (the blink does not move it)");

    // The reasoning's duration is the frame's, and it stops when the answer began.
    const model::StreamState& clock = fixture.state.ui.thread.stream;
    const long long expectedEnd =
        kT0 + static_cast<long long>(kReasoningUpdates + 1) * kStreamStepMs;
    check(reasoningEndedAt == expectedEnd,
          "the reasoning ended when the answer's text arrived (got " +
              toString(reasoningEndedAt - kT0) + " ms, expected " + toString(expectedEnd - kT0) +
              ")");
    check(clock.thinkingElapsedMs == static_cast<long long>(kReasoningUpdates) * kStreamStepMs,
          "and the row keeps the duration it took (" + toString(clock.thinkingElapsedMs) + " ms)");

    // Within one revision a *shorter* draw must not shrink the hint either: a disclosure toggled
    // at unchanged text is the case (and it is exactly what the anchor's next frame sees).
    const float before = fixture.state.threadAdapter.measureHint(groupId);
    fixture.state.threadAdapter.setBlockExpanded(fixture.streamGroupIndex(), 0, false);
    fixture.draw();
    const float after = fixture.state.threadAdapter.measureHint(groupId);
    check(after >= before - 0.01f, "the hint does not shrink within a revision (" +
                                       std::to_string(before) + " -> " + std::to_string(after) +
                                       ")");
    fixture.state.threadAdapter.setBlockExpanded(fixture.streamGroupIndex(), 0, true);
}

// ---------------------------------------------------------------------------
// 5. The caret, drawn
// ---------------------------------------------------------------------------

/// The caret's glyph as the text grid draws it (`Icon::Rule`'s Unicode form, "▌").
constexpr uint32_t kCaret = 0x258C;

void checkCaretDrawn() {
    phase("caret");
    Fixture fixture;
    fixture.addStored(message(1, tui::ChatMessageType::User, "[1] hello [0.1s]", "hello"));

    // A marked answer with the caret in its on half.
    fixture.stream("The live answer is still being written", "", kT0);
    Grid grid = fixture.draw();
    check(grid.countCell(kCaret) == 1, "the live answer draws exactly one caret (got " +
                                           std::to_string(grid.countCell(kCaret)) + ")");
    const int caretRow = [&grid]() {
        for (int y = 0; y < grid.ny; ++y)
            if (grid.rowContains(y, "\xE2\x96\x8C"))
                return y;
        return -1;
    }();
    check(caretRow >= 0 && grid.rowContains(caretRow, "written"),
          "the caret is on the body's last line (row " + std::to_string(caretRow) + ")");

    // The off half of the blink: no caret, and the *drawn* row does not move. `measureHint()` is
    // the wrong instrument here - the no-shrink rule pins it while the revision is unchanged, so
    // it would pass even if the glyph wrapped differently - so this reads the captured grid: the
    // same last row, the same rows of text, and the caret's cell holding a blank instead.
    fixture.tick(kT0 + model::kStreamCaretHalfPeriodMs);
    Grid offGrid = fixture.draw();
    check(offGrid.countCell(kCaret) == 0, "the blink's off half draws no caret");
    check(offGrid.lastInkRow() == grid.lastInkRow(),
          "and the drawn content keeps its last row (" + std::to_string(grid.lastInkRow()) +
              " vs " + std::to_string(offGrid.lastInkRow()) + ")");
    std::string caretRowOn = grid.rowText(caretRow);
    const std::size_t caretAt = caretRowOn.find("\xE2\x96\x8C");
    if (caretAt != std::string::npos)
        caretRowOn.replace(caretAt, 3, 1, ' ');
    check(caretRow >= 0 && offGrid.rowText(caretRow) == caretRowOn,
          "and the caret's own row is the row that was drawn: the blink changes one cell");

    // Before a single character arrives there is nothing to put a caret after.
    Fixture empty;
    empty.stream("", "reasoning only", kT0);
    Grid emptyGrid = empty.draw();
    check(emptyGrid.countCell(kCaret) == 0, "a body-less stream draws no caret");
    check(emptyGrid.contains("thinking"), "its row says the model is still thinking");

    // A cancelled answer keeps its text and loses the caret.
    Fixture stopped;
    stopped.stream("The answer that was cut", "", kT0);
    stopped.tick(kT0 + 7000, /*cancelRequested=*/true);
    Grid stoppedGrid = stopped.draw();
    check(stoppedGrid.countCell(kCaret) == 0, "a cancelled answer has no caret");
    check(stoppedGrid.contains("The answer that was cut"), "but keeps every character it had");

    // The committed answer is a stored message again: no stream, no caret.
    Fixture finished;
    finished.stream("The answer that finished", "", kT0);
    tui::tuiStreamChatMessageClear(finished.state);
    finished.addStored(message(2, tui::ChatMessageType::FinalAnswer, "[2] answer [1.0s]",
                               "The answer that finished"));
    Grid finishedGrid = finished.draw();
    check(finishedGrid.countCell(kCaret) == 0, "a finished answer has no caret");
    check(finishedGrid.contains("The answer that finished"), "and it is drawn as a stored message");
}

/// The pure half of the caret: `ui::withStreamingCaret()` (the fence rule is markdown's).
void checkCaretAppend() {
    phase("caret-append");
    const std::string caret = ui::icon(ui::Icon::Rule, ui::GlyphSet::Glyphs);
    check(caret == "\xE2\x96\x8C", "the caret's glyph is the icon table's `▌`");
    check(std::string(ui::icon(ui::Icon::Rule, ui::GlyphSet::Ascii)) == "|",
          "and the ASCII column's `|` (design FR-08's fallback)");
    check(ui::withStreamingCaret("", caret).empty(), "no body, no caret");
    check(ui::withStreamingCaret("abc", caret) == "abc" + caret,
          "the caret continues the last line");
    check(ui::withStreamingCaret("abc\n\n", caret) == "abc" + caret,
          "trailing line breaks do not open a line for it");
    check(ui::withStreamingCaret("a\n```", caret) == "a\n```\n" + caret,
          "a fence delimiter is never appended to (that would swallow the thread)");
    check(ui::withStreamingCaret("```py\nprint(1", caret) == "```py\nprint(1" + caret,
          "inside a fence the caret continues the code line");
    check(ui::withStreamingCaret("abc", " ") == "abc ", "the blink's off half is a space");

    // A table row is the second shape that is never appended to: a glyph after the row's last pipe
    // changes how the row and the delimiter row parse, so the caret goes on a line of its own -
    // after a blank line, because both readers run a table on while its lines look like rows and
    // md4c takes *any* non-blank line (a caret directly under a row would be an empty row of it).
    const std::string table = "| a | b |\n| --- | --- |";
    const std::string withRowCaret = ui::withStreamingCaret(table, caret);
    check(withRowCaret == table + "\n\n" + caret,
          "a table row puts the caret after a blank line (got '" + withRowCaret + "')");
    check(ui::findGfmTables(table + caret).empty(),
          "appending to the row would stop it being a table (the hazard the rule removes)");
    const auto rowSpans = ui::findGfmTables(withRowCaret);
    check(rowSpans.size() == 1 && rowSpans.back().end == table.size(),
          "with the caret on its own line the table is still a table and the caret is outside it");
    const std::string bodyRows = "| a | b |\n| --- | --- |\n| 1 | 2 |";
    check(ui::withStreamingCaret(bodyRows, caret) == bodyRows + "\n\n" + caret,
          "and the same for a body row");

    // GFM does not require the outer pipes, so `a | b` is a row too: the rule is the scanner's
    // answer, not "does the line start or end with a pipe".
    const std::string pipeLess = "a | b\n--- | ---";
    const std::string pipeLessCaret = ui::withStreamingCaret(pipeLess, caret);
    check(pipeLessCaret == pipeLess + "\n\n" + caret,
          "a pipe-less delimiter row gets the same own line (got '" + pipeLessCaret + "')");
    check(ui::findGfmTables(pipeLess + caret).empty(),
          "appending to it stops the table (the defect this rule closes)");
    const auto pipeLessSpans = ui::findGfmTables(pipeLessCaret);
    check(pipeLessSpans.size() == 1 && pipeLessSpans.back().end == pipeLess.size(),
          "and the pipe-less table survives the caret");
    const std::string pipeLessBody = "a | b\n--- | ---\n1 | 2";
    check(ui::withStreamingCaret(pipeLessBody, caret) == pipeLessBody + "\n\n" + caret,
          "a pipe-less body row too");
    // The own line keeps the last line's indentation: a table in a list item keeps its caret in
    // the item instead of starting a paragraph at the left margin.
    const std::string nested = "- item\n\n    | a | b |\n    | --- | --- |";
    const std::string nestedCaret = ui::withStreamingCaret(nested, caret);
    check(nestedCaret == nested + "\n\n    " + caret,
          "a nested table's caret line keeps the container's indent (got '" + nestedCaret + "')");

    // The other half of the rule: a line that merely *carries* a pipe is not a table row, so the
    // caret stays on it. (Dropping the old outer-pipe gate instead would move it onto a line of
    // its own here.)
    const std::string prose = "use a | b as a separator";
    check(ui::withStreamingCaret(prose, caret) == prose + caret,
          "prose with a top-level pipe keeps the caret on its line (got '" +
              ui::withStreamingCaret(prose, caret) + "')");
    const std::string code = "```\nls | grep x";
    check(ui::withStreamingCaret(code, caret) == code + caret, "and so does a pipe inside a fence");

    // A thematic break / setext underline is the third: `---` plus a glyph is a paragraph and
    // `- - -` plus one is a list.
    check(ui::withStreamingCaret("text\n---", caret) == "text\n---\n" + caret,
          "a rule line puts the caret on the next line");
    check(ui::withStreamingCaret("text\n- - -", caret) == "text\n- - -\n" + caret,
          "a spaced thematic break gets the same line (got '" +
              ui::withStreamingCaret("text\n- - -", caret) + "')");
    check(ui::withStreamingCaret("Heading\n===", caret) == "Heading\n===\n" + caret,
          "and so does a setext underline");

    // The ASCII column's caret is a separator itself: on a line of its own it is one more row of
    // the table above, and appended to a prose line right after a table it drags that line into
    // it - the blank line the table rule writes is what keeps both paragraphs.
    const std::string asciiCaret = ui::icon(ui::Icon::Rule, ui::GlyphSet::Ascii);
    check(asciiCaret == "|", "the ASCII caret is the pipe");
    const std::string asciiAfterTable = ui::withStreamingCaret(table, asciiCaret);
    check(asciiAfterTable == table + "\n\n|",
          "the ASCII caret gets its own paragraph after a table (got '" + asciiAfterTable + "')");
    const auto asciiSpans = ui::findGfmTables(asciiAfterTable);
    check(asciiSpans.size() == 1 && asciiSpans.back().end == table.size(),
          "so it is not an empty row of that table");
    const std::string proseAfterTable = table + "\nand that is all";
    const std::string asciiAfterProse = ui::withStreamingCaret(proseAfterTable, asciiCaret);
    const auto proseSpans = ui::findGfmTables(asciiAfterProse);
    check(proseSpans.size() == 1 && proseSpans.back().end == table.size(),
          "and a prose line after a table is not dragged into it (got '" + asciiAfterProse + "')");
}

// ---------------------------------------------------------------------------
// 6. The header: phase, clock and the reasoning's duration
// ---------------------------------------------------------------------------

void checkHeader() {
    phase("header");
    Fixture fixture;
    fixture.stream("", "Let me think", kT0);
    check(headerOf(fixture, 0) == "Thinking",
          "a reasoning row prints no duration while it reasons (got '" + headerOf(fixture, 0) +
              "')");
    check(headerOf(fixture, 1) == "AGENT - thinking",
          "and the body row says the model is still thinking (got '" + headerOf(fixture, 1) + "')");
    // The answer's clock runs while the model reasons too (FR-08's elapsed time), so the row is
    // `AGENT · thinking · 00:02` a moment in - the phase word is what changes, never the clock's
    // absence.
    fixture.tick(kT0 + 2400);
    check(headerOf(fixture, 1) == "AGENT - thinking - 00:02",
          "the run's clock is drawn beside the thinking phase as well (got '" +
              headerOf(fixture, 1) + "')");

    // The answer begins 3.2 s in and the frame lands 12 s in.
    fixture.stream("The answer", "Let me think", kT0 + 3200);
    fixture.stream("The answer", "Let me think", kT0 + 12000);
    check(headerOf(fixture, 0) == "Thinking - 3.2 s",
          "once the reasoning ends its row keeps the duration (got '" + headerOf(fixture, 0) +
              "')");
    check(headerOf(fixture, 1) == "AGENT - writing - 00:12",
          "the live row prints `AGENT · writing · mm:ss` (got '" + headerOf(fixture, 1) + "')");

    // A stop freezes the clock and names what happened.
    fixture.tick(kT0 + 19000, /*cancelRequested=*/true);
    check(headerOf(fixture, 1) == "AGENT - cancelled - 00:19",
          "a stopped answer says so and keeps its clock (got '" + headerOf(fixture, 1) + "')");
    check(headerOf(fixture, 0) == "Thinking - 3.2 s",
          "the reasoning's row is unchanged by the stop (got '" + headerOf(fixture, 0) + "')");

    // The header is the block's, so a stored message keeps its own shape.
    Fixture stored;
    stored.addStored(message(1, tui::ChatMessageType::FinalAnswer, "[1] done [2.5s]", "done"));
    const model::BlockDesc desc = stored.state.threadAdapter.blockAt(0, 0);
    check(views::blockHeaderText(desc, views::BlockDraw{true, false}, true) == "ANSWER - 2.5 s",
          "a stored answer's header is a duration, not a phase (got '" +
              views::blockHeaderText(desc, views::BlockDraw{true, false}, true) + "')");
}

// ---------------------------------------------------------------------------
// 8. The live answer is drawn after the bounded window, not inside it
// ---------------------------------------------------------------------------

void checkWindow() {
    phase("window");
    Fixture fixture;
    for (int i = 1; i <= 100; ++i) {
        fixture.addStored(
            message(static_cast<size_t>(i), tui::ChatMessageType::User,
                    "[" + std::to_string(i) + "] question " + std::to_string(i) + " [0.1s]",
                    "question " + std::to_string(i)));
    }
    fixture.stream("the live answer", "", kT0);
    check(fixture.state.threadAdapter.groupCount() == 101, "100 stored groups and the live answer");
    check(fixture.state.threadAdapter.storedGroupCount() == 100,
          "the window counts the stored conversation only (got " +
              std::to_string(fixture.state.threadAdapter.storedGroupCount()) + ")");
    check(fixture.state.threadAdapter.hasStream(), "the thread knows it ends with a live answer");

    const views::ThreadWindow window =
        views::threadWindow(fixture.state.threadAdapter.storedGroupCount(), 3, 0);
    check(window.hidden == 97,
          "the paging count is the stored groups' (97, got " + std::to_string(window.hidden) + ")");
    check(window.count == 3, "the window itself is the three newest stored groups");

    fixture.draw(/*windowGroups=*/3);
    check(fixture.state.ui.thread.drawnGroups == 4,
          "three stored groups plus the live answer are drawn (got " +
              std::to_string(fixture.state.ui.thread.drawnGroups) + ")");
    // The live answer was drawn last: it is the newest group, and its id is the newest id.
    const store::GroupView stream = fixture.streamGroup();
    check(stream.id == fixture.state.threadAdapter.groupAt(100).id,
          "the live answer is the thread's last group");
    check(stream.revision > 0,
          "and it carries the stream's revision (got " + std::to_string(stream.revision) + ")");

    // The same thread with no stream draws exactly the window.
    Fixture noStream;
    for (int i = 1; i <= 100; ++i) {
        noStream.addStored(message(static_cast<size_t>(i), tui::ChatMessageType::User,
                                   "[" + std::to_string(i) + "] q" + std::to_string(i) + " [0.1s]",
                                   "q" + std::to_string(i)));
    }
    noStream.draw(/*windowGroups=*/3);
    check(noStream.state.ui.thread.drawnGroups == 3,
          "without a stream the window alone is drawn (got " +
              std::to_string(noStream.state.ui.thread.drawnGroups) + ")");
}

// ---------------------------------------------------------------------------
// 10. The cost of one streaming frame (NFR-02: TUI <= 8 ms)
// ---------------------------------------------------------------------------

void checkCost() {
    phase("cost");
    const std::string document = fixtureDocument();
    Fixture fixture;
    fixture.addStored(message(1, tui::ChatMessageType::User, "[1] stream [0.1s]", "stream"));
    fixture.stream(document, "reasoning", kT0);
    // Warm up (the atlas and rich_md's cache), then measure the best of five draws: a loaded
    // machine must not turn a budget check into a coin toss.
    fixture.draw();
    double best = 1.0e9;
    for (int i = 0; i < 5; ++i) {
        const auto start = std::chrono::steady_clock::now();
        fixture.draw();
        const auto stop = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(stop - start).count();
        best = std::min(best, ms);
    }
    check(best <= 8.0, "one streaming frame stays within NFR-02 (8 ms, measured " +
                           std::to_string(best) + " ms)");
    if (g_verbose)
        std::printf("     streaming frame: %.2f ms (best of 5, %zu-byte answer)\n", best,
                    document.size());
}

// ---------------------------------------------------------------------------
// 11. The goldens
// ---------------------------------------------------------------------------

struct GoldenScene {
    const char* name;
    std::function<void(Fixture&)> build;
};

std::vector<GoldenScene> scenes() {
    std::vector<GoldenScene> list;
    list.push_back({"live", [](Fixture& f) {
                        f.stream("", "Read the request.", kT0);
                        f.stream("The answer is **still** arriving.\n\nA second paragraph, so the "
                                 "caret has a line to sit on.",
                                 "Read the request.", kT0 + 3200);
                        f.stream("The answer is **still** arriving.\n\nA second paragraph, so the "
                                 "caret has a line to sit on.",
                                 "Read the request.", kT0 + 12000);
                    }});
    list.push_back({"cancelled", [](Fixture& f) {
                        f.stream("The answer was cut here", "Read the request.", kT0);
                        f.stream("The answer was cut here", "Read the request.", kT0 + 3200);
                        f.tick(kT0 + 7000, /*cancelRequested=*/true);
                    }});
    list.push_back({"table", [](Fixture& f) {
                        // The revision every streamed table passes through: the last line is a
                        // table row, so the caret must not be appended to it (review finding 2) -
                        // the grid still draws and the caret gets a row of its own, one blank
                        // line below the table. The blank line is what ends the table for md4c,
                        // and on the grid the block gap it opens is under a cell, so the caret
                        // keeps the row it had.
                        f.stream("| Tick | Text | Caret |\n| ---- | ---- | ----- |\n"
                                 "| 1 | `Streaming v2` | on |",
                                 "", kT0);
                        f.tick(kT0 + 3000);
                    }});
    list.push_back({"thinking", [](Fixture& f) {
                        f.stream("", "Working out what to say next.", kT0);
                        f.tick(kT0 + 2400);
                    }});
    return list;
}

std::string goldenText(const char* name, const Grid& grid) {
    std::string text = "name: ";
    text += name;
    text += "\nscreen: ";
    text += std::to_string(grid.nx);
    text += "x";
    text += std::to_string(grid.ny);
    text += "\ntext:\n";
    for (int y = 0; y < grid.ny; ++y)
        text += grid.rowText(y) + "\n";
    text += "inks:\n";
    for (int y = 0; y < grid.ny; ++y) {
        std::string row;
        for (int x = 0; x < grid.nx; ++x) {
            const uint8_t ink = grid.fgAt(x, y);
            row += (ink < 10 ? "0" : "") + std::to_string(static_cast<int>(ink)) + " ";
        }
        while (!row.empty() && row.back() == ' ')
            row.pop_back();
        text += row + "\n";
    }
    return text;
}

void checkGoldens() {
    for (const GoldenScene& scene : scenes()) {
        ::setenv("LLMFUN_ASCII", "0", 1);
        ui::applyTheme(ui::ThemeId::Graphite, ui::ColorDepth::TrueColor);
        Fixture fixture;
        scene.build(fixture);
        Grid grid;
        for (int i = 0; i < 3;
             ++i) // settle: a code block's child window measures itself a frame later
            grid = fixture.draw();
        if (g_dump) {
            std::fprintf(
                stderr, "--- %s (view=%d groups=%zu stored=%zu drawn=%zu, hint=%.1f) ---\n",
                scene.name, static_cast<int>(fixture.state.ui.view),
                fixture.state.threadAdapter.groupCount(),
                fixture.state.threadAdapter.storedGroupCount(), fixture.state.ui.thread.drawnGroups,
                fixture.state.threadAdapter.measureHint(fixture.streamGroup().id));
            std::printf("--- %s ---\n%s", scene.name, grid.dump().c_str());
        }
        const std::string text = goldenText(scene.name, grid);
        const std::string path =
            std::string(TEST_STREAMING_GOLDEN_DIR) + "/streaming-" + scene.name + "-100x30.txt";
        if (g_update) {
            std::ofstream out(path);
            out << text;
            out.close();
            check(out.good(), "golden written: " + path);
            continue;
        }
        std::ifstream in(path);
        check(in.good(), "golden is readable: " + path + " (run with --update to create it)");
        if (!in.good())
            continue;
        const std::string want((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        check(want == text, std::string("the ") + scene.name + " golden matches");
        if (want != text && g_verbose) {
            std::size_t i = 0;
            while (i < want.size() && i < text.size() && want[i] == text[i])
                ++i;
            int line = 1;
            for (std::size_t k = 0; k < i; ++k)
                line += want[k] == '\n' ? 1 : 0;
            std::fprintf(stderr, "  first difference on line %d\n", line);
        }
    }
    ::setenv("LLMFUN_ASCII", "0", 1);
    ui::applyTheme(ui::ThemeId::Graphite, ui::ColorDepth::TrueColor);
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0)
            g_verbose = true;
        else if (std::strcmp(argv[i], "--update") == 0)
            g_update = true;
        else if (std::strcmp(argv[i], "--dump") == 0)
            g_dump = true;
        else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0)
            usage(argv[0], "print every check with -v, rewrite the goldens with --update");
        else
            usage(argv[0], "unknown argument");
    }

    // One glyph set and one colour depth for the run, whatever terminal it starts on.
    ::setenv("LLMFUN_ASCII", "0", 1);
    ::setenv("COLORTERM", "truecolor", 1);
    tui::TuiState state;
    state.iniFilename = "";
    harnessInit(state);
    frame(
        [](const ui::Metrics&) {}); // warm-up: the text backend builds its atlas on the first frame

    checkSource();
    checkClock();
    checkThrottle();
    checkCaretAppend();
    checkHeader();
    checkCaretDrawn();
    checkIdentityAndHeight();
    checkWindow();
    checkCost();
    checkGoldens();

    harnessShutdown(state);

    if (g_failures == 0) {
        std::printf("test_streaming: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "test_streaming: %d of %d checks failed\n", g_failures, g_checks);
    return 1;
}
