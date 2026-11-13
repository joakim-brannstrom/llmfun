// test_streaming.cpp
//
// Streaming: the live answer made legible and cheap, on the typed store.
//
// What this binary proves, in order:
//
//   1. the sources   the streaming state is the frame's (`refreshStreamClock` is called once per
//                    frame in `views/shell.cpp`), no view names a v4 type, a profile flag or a
//                    display size, the bounded window counts the *stored* groups
//                    (`storedGroupCount`), and no production source names a retired fold symbol
//                    or a pipeline literal (the gate, a closed file list).
//   2. the clock     a fake clock drives every number: the answer's elapsed time and the
//                    reasoning's duration (0 while it is still reasoning, frozen when the answer
//                    begins).
//   3. the storm     300 stream ticks of one live turn merge into one turn - the query and the
//                    content block - and one draw rebuilds the group once; a stop stores the turn.
//   4. the identity  across 300 updates of a markdown-heavy answer the stream group's id and its
//                    content block (`{group, hash("content#0"), Assistant}`) never move, and the
//                    group's `revision` advances exactly when the text does.
//   5. the header    the live turn is the work group D stamps: `WORK - N steps` over its rows, the
//                    reasoning row reads `Thinking` until D stamps its duration at completion, and
//                    a stamped block keeps its duration (`ANSWER - 2.5 s`, `Thinking - 2.5 s`).
//   6. the height    within one revision the group's height never shrinks, so a live answer
//                    cannot oscillate; folding the live group's header shortens it (minimal mode
//                    honors the fold).
//   7. the window    the live answer is drawn after the bounded window, not inside it: three
//                    stored groups plus the stream is four drawn groups and the paging count is
//                    the stored conversation's.
//   8. the cost      one streaming frame over a 100 KB answer stays inside the 8 ms text-grid
//   budget.
//   9. the goldens   the live answer, the cancelled answer, a reasoning-only stream, a stream
//                    whose last line is a table row, and a stream inside an unclosed fence.
//  10. the fallback  a stored body never takes the plain-text fallback; the live turn's folded
//                    body is its row's summary, drawn as the text arrived.
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

#include "test/typed_seed.h" // the typed store's seeding shapes (global scope: the
                             // seeder must name the real `llmfun::tui::TuiState`)

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

std::string toString(std::int64_t value) { return std::to_string(value); }

// ---------------------------------------------------------------------------
// The cell grid the text backend writes
// ---------------------------------------------------------------------------

ImTui::TScreen g_screen;

constexpr int kScreenW = 100;
constexpr int kScreenH = 30;

/// The fake clock every clock check uses: the checks state their offsets outright (`kT0 + 3200`,
/// say), so what a reading means depends on nothing the wall clock decides.
constexpr std::int64_t kT0 = 1000000;

/// How much of the answer's clock one fixture update advances (the stream test's cadence).
constexpr std::int64_t kStreamStepMs = 4000;

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
    typed_seed::Seeder seed{&state, 1}; ///< the typed store's live-run seeding

    Fixture() {
        state.readyStatus = false; // a run is in flight: the composer's Stop is up
        state.ui.view = model::ThreadView::Ready;
        state.threadAdapter.bind(state);
    }

    /// The typed store's seeding shapes: a stored user message opens a `Done` turn
    /// with its query block, a stored answer hangs the turn's `Answer` block off
    /// the newest turn.
    void storedUser(const std::string& text) { seed.add(typed_seed::Seeder::Msg::User, text); }
    void storedAnswer(const std::string& text) {
        seed.add(typed_seed::Seeder::Msg::FinalAnswer, text);
    }

    /// Put a streamed answer in the typed store and derive its clock at `nowMs` - the
    /// same two writes the emitters and the frame layer make, with the clock stated
    /// instead of read. The first call opens the live turn (a `Running` turn with the
    /// query block), every later one merges the content block in place.
    void stream(const std::string& text, const std::string& thinking, std::int64_t nowMs,
                bool cancelRequested = false) {
        if (seed.liveTurnId == 0)
            seed.runStart("stream this");
        seed.updateStream(text);
        if (!thinking.empty())
            seed.upsertBlock(seed.liveTurnId, "reasoning#0", std::to_string(seed.liveTurnId) + "#0",
                             TuiBlockKind_Reasoning, TuiBlockStatus_None, nullptr, thinking);
        model::refreshStreamClock(state.ui.thread.stream, state.threadAdapter.hasStream(),
                                  state.threadAdapter.streamHasText(),
                                  state.threadAdapter.streamHasThinking(), nowMs, cancelRequested);
    }

    /// Advance the clock without touching the text (a frame that changed nothing).
    void tick(std::int64_t nowMs, bool cancelRequested = false) {
        model::refreshStreamClock(state.ui.thread.stream, state.threadAdapter.hasStream(),
                                  state.threadAdapter.streamHasText(),
                                  state.threadAdapter.streamHasThinking(), nowMs, cancelRequested);
    }

    size_t streamGroupIndex() const { return state.threadAdapter.storedGroupCount(); }
    store::GroupView streamGroup() const { return state.threadAdapter.groupAt(streamGroupIndex()); }

    /// The stream group's content row: the `Assistant`-kind block D's stream updater
    /// merges the answer into (`content#0`; the query block is a `User` row, the
    /// reasoning a `Thinking` one). The batch's synthetic owner row is
    /// an `Assistant` row too, so the owner's namespaced key is skipped - the
    /// helper names the streamed body's row, never the owner's. A body-less
    /// answer is dropped by the projection, so the row need not exist
    /// yet; the helper reports `npos` rather than pointing at a stand-in.
    static constexpr size_t npos = static_cast<size_t>(-1);

    size_t streamBodyRow() const {
        const size_t blocks = state.threadAdapter.blockCount(streamGroupIndex());
        const std::size_t ownerKey = std::hash<std::string>{}(
            "batch#" + std::to_string(seed.liveTurnId) + "#" + std::to_string(seed.liveBatch));
        for (size_t b = 1; b < blocks; ++b)
            if (state.threadAdapter.blockAt(streamGroupIndex(), b).kind ==
                    model::BlockKind::Assistant &&
                state.threadAdapter.blockAt(streamGroupIndex(), b).key != ownerKey)
                return b;
        return npos;
    }

    /// The stream group's reasoning row, found by kind - the batch projection
    /// emits the owner first and sorts the children stably, so a
    /// literal index would not hold across the batch emission order.
    size_t streamReasoningRow() const {
        const size_t blocks = state.threadAdapter.blockCount(streamGroupIndex());
        for (size_t b = 1; b < blocks; ++b)
            if (state.threadAdapter.blockAt(streamGroupIndex(), b).kind ==
                model::BlockKind::Thinking)
                return b;
        return npos;
    }

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
    return views::blockHeaderText(desc, views::BlockDraw{desc.expanded, false},
                                  ui::metricsFor(/*textGrid=*/true, 1.0f, 1.0f), 1000.0f);
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
}

// The gate: a retired fold symbol or a wire literal never returns to
// the production tree. Production sources only: a fixture may name a hidden
// tool (that is what a fixture is for), a shipped source may not - so the scan
// is this closed file list, never a recursive walk. A new file needs a new row
// here: the list is the set of files where these tokens last lived - the
// paired headers and the rest of the production tree are a follow-up wave's
// extension, not this gate's closure. `groupMinimal` is on the list because
// the split into `groupOpen`/`groupStyleIsMinimal` is the point - a
// returning `groupMinimal` would silently re-conflate style with openness.
void checkRetiredNames() {
    const std::string dir = TEST_STREAMING_SOURCE_DIR;
    const char* kProduction[] = {"store/thread_store.cpp",
                                 "store/thread_store.h",
                                 "store/typed_thread_store.cpp",
                                 "store/typed_thread_store.h",
                                 "views/thread.cpp",
                                 "views/blocks.cpp",
                                 "views/palette.cpp",
                                 "model/block.h",
                                 "tui.h",
                                 "tui_api.cpp"};
    const char* kRetired[] = {
        "thinkingOpen",     "batchTools", "groupOverrides", "openWorkGroups",  "typedRowOpen_",
        "kHiddenToolNames", "task_done",  "taskDone",       "pipeline_output", "groupMinimal"};
    for (const char* file : kProduction) {
        std::string source;
        const bool readable = readWholeFile(dir + "/" + file, source);
        check(readable && !source.empty(), std::string("readable: ") + file);
        if (!readable || source.empty())
            continue;
        for (const char* token : kRetired)
            check(source.find(token) == std::string::npos,
                  std::string(file) + " names no retired token (" + token + ")");
    }
}

// ---------------------------------------------------------------------------
// 2. The clock: elapsed and the reasoning's duration - with a fake clock
// ---------------------------------------------------------------------------

void checkClock() {
    phase("clock");
    model::StreamState s;
    check(!s.active(), "a fresh clock is inactive");

    model::refreshStreamClock(s, /*hasStream=*/false, false, false, kT0, false);
    check(!s.active() && s.elapsedMs == 0, "no stream answers 00:00");

    // The first sighting of a stream *is* its start.
    model::refreshStreamClock(s, /*hasStream=*/true, /*hasText=*/false, /*hasThinking=*/true, kT0,
                              false);
    check(s.startedMs == kT0, "the first sighting starts the clock");
    check(s.elapsedMs == 0, "the answer begins at 00:00");
    check(s.thinkingElapsedMs == 0, "the reasoning's duration is 0 while it is still reasoning");

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

    // A stop freezes the clock where the text stopped arriving.
    model::refreshStreamClock(s, true, true, true, kT0 + 27000, /*cancelRequested=*/true);
    check(s.cancelled, "a cancel request marks the answer cancelled");
    check(s.elapsedMs == 27000,
          "the clock freezes at the stop (got " + toString(s.elapsedMs) + ")");
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
    check(!ended.active() && ended.elapsedMs == 0 && !ended.cancelled,
          "the stream's end clears the clock and the cancel mark");
}

// ---------------------------------------------------------------------------
// 3. The store's upsert storm: N ticks of one live turn merge in place
// ---------------------------------------------------------------------------

void checkThrottle() {
    phase("storm");
    Fixture fast;
    fast.seed.runStart("stream this");
    fast.state.threadAdapter.storedGroupCount(); // prime: the store holds the run's start
    // Minimal folds the live turn's work rows behind its header: pin the
    // group open so the streamed body draws, and draw once so the pinned-open
    // group's own rebuild is spent before the storm the check below measures.
    fast.state.chat.groupPins[static_cast<std::int64_t>(fast.seed.liveTurnId)] = true;
    fast.draw();
    const auto buildsBefore = fast.state.threadAdapter.groupBuilds();
    // 300 ticks as fast as D's loop sends them (no frame between): the store merges every
    // one in place - the live turn still holds its query and its content block, and the
    // whole storm costs one rebuild of the group.
    for (int i = 1; i <= 300; ++i)
        fast.seed.updateStream("chunk " + std::to_string(i));
    const Grid grid = fast.draw();
    check(fast.state.threadAdapter.groupBuilds() == buildsBefore + 1,
          "300 ticks merge into one rebuild of the group (got " +
              std::to_string(fast.state.threadAdapter.groupBuilds() - buildsBefore) + ")");
    const size_t group = fast.streamGroupIndex();
    check(fast.state.threadAdapter.blockCount(group) == 3,
          "the storm still leaves the turn its header, the batch's owner row and one "
          "content block (got " +
              std::to_string(fast.state.threadAdapter.blockCount(group)) + ")");
    check(grid.contains("chunk 300"), "the drawn body is the newest tick");

    // A stop closes the turn: the group is stored, the thread's live answer is gone.
    fast.seed.runStop();
    check(!fast.state.threadAdapter.hasStream(), "the stop closes the live turn");
    check(fast.state.threadAdapter.storedGroupCount() == 1,
          "and the turn is stored as an ordinary group (got " +
              std::to_string(fast.state.threadAdapter.storedGroupCount()) + ")");
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
    fixture.storedUser("stream this");
    // The reasoning arrives first (40 updates) and ends when the answer's first character does.
    constexpr int kReasoningUpdates = 40;

    bool identityStable = true;
    bool revisionMonotone = true;
    bool heightStableWithinRevision = true;
    bool sawBody = false;
    std::uint64_t previousRevision = 0;
    size_t groupId = 0;
    std::int64_t now = kT0;
    std::int64_t reasoningEndedAt = 0;

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
        // Four seconds of the answer's clock per update: the height samples below span several
        // readings of one revision and the clock only ever moves forward - the derivation treats
        // a reading older than the last one as the same moment (a wall clock can step back). One
        // update per four seconds is slow for a stream and right for a clock test.
        now = kT0 + static_cast<std::int64_t>(step) * kStreamStepMs;
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
        // The content row keeps its identity: the block D's stream updater merges
        // the answer into (`content#0`), whatever the reasoning around it does.
        // A body-less answer is dropped by the projection, so the
        // reasoning-only updates have no content row at all; once the text has
        // arrived the row must be there, carry `content#0`'s id, and never
        // disappear again.
        const size_t bodyRow = fixture.streamBodyRow();
        if (bodyRow != Fixture::npos) {
            const model::BlockDesc body =
                fixture.state.threadAdapter.blockAt(fixture.streamGroupIndex(), bodyRow);
            identityStable =
                identityStable &&
                body.id() == model::BlockId{groupId, std::hash<std::string>{}("content#0"),
                                            model::BlockKind::Assistant};
            sawBody = true;
        } else if (sawBody)
            identityStable = false; // the row must not vanish once it has text
        if (step > 1 && view.revision < previousRevision)
            revisionMonotone = false;
        previousRevision = view.revision;

        // The height within this revision. A markdown construct (a fenced code block, a bridged
        // table) needs a few frames to settle, so what must never happen is a *shrink* (the row
        // would move under the reader) and it must stop growing. Six samples 100 ms apart read
        // the same revision at several clock readings.
        constexpr int kSamples = 6;
        float heights[kSamples] = {};
        for (int pass = 0; pass < kSamples; ++pass) {
            fixture.draw();
            heights[pass] = fixture.state.threadAdapter.measureHint(groupId);
            fixture.tick(now + static_cast<std::int64_t>(pass) * 100);
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

    check(sawBody, "the content row exists once the answer's text arrives");
    check(identityStable,
          "300 updates keep the stream group's id and the content block's id stable");
    check(groupId != 0, "the stream group has an id (got " + std::to_string(groupId) + ")");
    check(revisionMonotone, "the group's revision never goes backwards");
    check(previousRevision >= 250, "and it advanced with the text (final revision " +
                                       std::to_string(previousRevision) + ")");
    check(
        heightStableWithinRevision,
        "within one revision the height never shrinks and settles (the content does not move it)");

    // The reasoning's duration is the frame's, and it stops when the answer began.
    const model::StreamState& clock = fixture.state.ui.thread.stream;
    const std::int64_t expectedEnd =
        kT0 + static_cast<std::int64_t>(kReasoningUpdates + 1) * kStreamStepMs;
    check(reasoningEndedAt == expectedEnd,
          "the reasoning ended when the answer's text arrived (got " +
              toString(reasoningEndedAt - kT0) + " ms, expected " + toString(expectedEnd - kT0) +
              ")");
    check(clock.thinkingElapsedMs == static_cast<std::int64_t>(kReasoningUpdates) * kStreamStepMs,
          "and the row keeps the duration it took (" + toString(clock.thinkingElapsedMs) + " ms)");

    // The live group's hint is the row-count estimate, so it is expansion-independent:
    // folding the header changes the drawn height (the fold the render honors), not
    // the estimate the store hands the anchor. Pin the stability, then leave the
    // group open for the goldens that follow.
    const float before = fixture.state.threadAdapter.measureHint(groupId);
    fixture.state.threadAdapter.setBlockExpanded(fixture.streamGroupIndex(), 0, false);
    fixture.draw();
    const float after = fixture.state.threadAdapter.measureHint(groupId);
    check(after == before, "the hint is the estimate, and does not move with the fold (" +
                               std::to_string(before) + " -> " + std::to_string(after) + ")");
    fixture.state.threadAdapter.setBlockExpanded(fixture.streamGroupIndex(), 0, true);
}

// ---------------------------------------------------------------------------
// 5. The header: the phase word and the reasoning's duration
// ---------------------------------------------------------------------------

void checkHeader() {
    phase("header");
    Fixture fixture;
    fixture.stream("", "Let me think", kT0);
    check(headerOf(fixture, fixture.streamReasoningRow()) == "Thinking",
          "a reasoning row's header is the bare word while it streams (got '" +
              headerOf(fixture, fixture.streamReasoningRow()) + "')");
    check(headerOf(fixture, 0) == "WORK - 1 step",
          "and the live turn's header is the work group's (got '" + headerOf(fixture, 0) + "')");

    // The answer's text arrives: the reasoning row holds still (D stamps the duration
    // at completion), and seconds passing move nothing.
    fixture.stream("The answer", "Let me think", kT0 + 12000);
    check(headerOf(fixture, fixture.streamReasoningRow()) == "Thinking",
          "the reasoning row holds still while the answer streams (got '" +
              headerOf(fixture, fixture.streamReasoningRow()) + "')");
    fixture.tick(kT0 + 13000);
    fixture.tick(kT0 + 14000);
    fixture.tick(kT0 + 15000);
    check(headerOf(fixture, fixture.streamReasoningRow()) == "Thinking",
          "and holds still across three seconds of ticking (flicker-free)");

    // A stop closes the turn; the group is stored (the thread's only group) and keeps
    // the block labels D stamped.
    fixture.seed.runStop();
    const size_t group = 0;
    check(fixture.state.threadAdapter.blockAt(group, 0).label == "WORK",
          "the stored turn keeps its work header (got '" +
              fixture.state.threadAdapter.blockAt(group, 0).label + "')");
    // The reasoning row by kind, not position: the stored batch carries the
    // owner first and the sorted children after it.
    size_t storedThinking = Fixture::npos;
    const size_t storedBlocks = fixture.state.threadAdapter.blockCount(group);
    for (size_t b = 1; b < storedBlocks; ++b)
        if (fixture.state.threadAdapter.blockAt(group, b).kind == model::BlockKind::Thinking)
            storedThinking = b;
    check(storedThinking != Fixture::npos &&
              fixture.state.threadAdapter.blockAt(group, storedThinking).label == "Thinking",
          "and the reasoning row's header, still unstamped (got '" +
              (storedThinking != Fixture::npos
                   ? fixture.state.threadAdapter.blockAt(group, storedThinking).label
                   : "<no reasoning row>") +
              "')");

    // The header is the block's, so a completed block keeps its own shape: the Answer
    // block D stamps at completion prints its duration.
    Fixture stored;
    stored.seed.upsertTurn(1, TuiTurnState_Done, 1);
    stored.seed.upsertBlock(1, "answer", "1#0", TuiBlockKind_Answer, TuiBlockStatus_None, nullptr,
                            "done", nullptr, 2500);
    const model::BlockDesc desc = stored.state.threadAdapter.blockAt(0, store::kAnswerRow);
    check(views::blockHeaderText(desc, views::BlockDraw{true, false},
                                 ui::metricsFor(/*textGrid=*/true, 1.0f, 1.0f),
                                 1000.0f) == "ANSWER - 2.5 s",
          "a stamped answer block's header is its duration (got '" +
              views::blockHeaderText(desc, views::BlockDraw{true, false},
                                     ui::metricsFor(/*textGrid=*/true, 1.0f, 1.0f), 1000.0f) +
              "')");

    // A stamped reasoning drops its duration anyway: the row is the
    // bare word, the owner carries the cost.
    stored.seed.upsertBlock(1, "reasoning#0", "1#0", TuiBlockKind_Reasoning, TuiBlockStatus_None,
                            nullptr, "the reasoning", nullptr, 2500);
    const model::BlockDesc reasoning = stored.state.threadAdapter.blockAt(0, 2);
    check(reasoning.kind == model::BlockKind::Thinking, "a turn's reasoning is its own block");
    check(views::blockHeaderText(reasoning, views::BlockDraw{true, false},
                                 ui::metricsFor(/*textGrid=*/true, 1.0f, 1.0f),
                                 1000.0f) == "Thinking",
          "and a stamped reasoning row drops its duration too (got '" +
              views::blockHeaderText(reasoning, views::BlockDraw{true, false},
                                     ui::metricsFor(/*textGrid=*/true, 1.0f, 1.0f), 1000.0f) +
              "')");
}

// ---------------------------------------------------------------------------
// 6. The fence fallback: stored bodies never take it, streamed ones draw raw
// ---------------------------------------------------------------------------

void checkFenceFallback() {
    phase("fence fallback");
    // The typed live turn has no fence fallback to take: its body is D's merged
    // content block, and an opened live turn draws its body raw - the text as
    // it arrived, fence opener and all (minimal folds the work rows, so the
    // check pins the group open first).
    Fixture fixture;
    fixture.stream("## Heading\n\n- item\n\n```python\n", "", kT0);
    fixture.state.chat.groupPins[static_cast<std::int64_t>(fixture.seed.liveTurnId)] = true;
    Grid grid = fixture.draw();
    check(grid.contains("Heading") && grid.contains("item"),
          "a live body that ends inside a fence still draws - the renderer absorbs the open fence");

    // A stored body with an unterminated fence is a finished body: it belongs to
    // the markdown renderer whatever it holds, on every draw.
    Fixture stored;
    stored.storedAnswer("```python\nx = 1\n");
    stored.draw();
    const ui::MarkdownCounters first = ui::markdownCounters();
    stored.draw();
    const ui::MarkdownCounters second = ui::markdownCounters();
    check(second.renders > first.renders,
          "a stored body still goes through the markdown renderer every draw");
    check(second.plainTextFallbacks == first.plainTextFallbacks,
          "and a stored body never takes the plain-text fallback");
}

// ---------------------------------------------------------------------------
// 7. The live answer is drawn after the bounded window, not inside it
// ---------------------------------------------------------------------------

void checkWindow() {
    phase("window");
    Fixture fixture;
    for (int i = 1; i <= 100; ++i)
        fixture.storedUser("question " + std::to_string(i));
    fixture.stream("the live answer", "", kT0);
    fixture.stream("the live answer, grown", "", kT0 + kStreamStepMs);
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
    for (int i = 1; i <= 100; ++i)
        noStream.storedUser("q" + std::to_string(i));
    noStream.draw(/*windowGroups=*/3);
    check(noStream.state.ui.thread.drawnGroups == 3,
          "without a stream the window alone is drawn (got " +
              std::to_string(noStream.state.ui.thread.drawnGroups) + ")");
}

// ---------------------------------------------------------------------------
// 9. The cost of one streaming frame (TUI <= 8 ms)
// ---------------------------------------------------------------------------

void checkCost() {
    phase("cost");
    // The fixture document alone is far under the budget, so the measured body is the document
    // repeated until it crosses the large-body size: the streaming path must stay flat as the
    // answer grows, not merely cheap while it is small.
    const std::string base = fixtureDocument();
    std::string document = base;
    while (document.size() < 100 * 1024)
        document += base;
    Fixture fixture;
    fixture.storedUser("stream");
    fixture.stream(document, "reasoning", kT0);
    check(document.size() >= 100 * 1024, "the measured answer is a large body (got " +
                                             std::to_string(document.size()) + " bytes)");
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
    check(best <= 8.0, "one streaming frame over the large body stays within 8 ms (measured " +
                           std::to_string(best) + " ms)");
    if (g_verbose)
        std::printf("     streaming frame: %.2f ms (best of 5, %zu-byte answer)\n", best,
                    document.size());
}

// ---------------------------------------------------------------------------
// 10. The goldens
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
                                 "body has more than one line.",
                                 "Read the request.", kT0 + 3200);
                        f.stream("The answer is **still** arriving.\n\nA second paragraph, so the "
                                 "body has more than one line.",
                                 "Read the request.", kT0 + 12000);
                    }});
    list.push_back({"cancelled", [](Fixture& f) {
                        f.stream("The answer was cut here", "Read the request.", kT0);
                        f.stream("The answer was cut here", "Read the request.", kT0 + 3200);
                        f.tick(kT0 + 7000, /*cancelRequested=*/true);
                    }});
    list.push_back({"table", [](Fixture& f) {
                        // The revision every streamed table passes through: the last line is a
                        // table row, so the body ends inside the table while it streams - and
                        // the table keeps being a table, on the grid as in the scanner.
                        f.stream("| Tick | Text | State |\n| ---- | ---- | ----- |\n"
                                 "| 1 | `Streaming v2` | on |",
                                 "", kT0);
                        f.tick(kT0 + 3000);
                    }});
    list.push_back({"thinking", [](Fixture& f) {
                        f.stream("", "Working out what to say next.", kT0);
                        f.tick(kT0 + 2400);
                    }});
    list.push_back({"fence", [](Fixture& f) {
                        // The revision that flips the body to plain text: the fence opener is
                        // the last line, so the heading and the list item above it hold the
                        // shape they arrived in instead of being re-classified mid-stream.
                        f.stream("## Heading\n\n- item\n\n```python\n", "", kT0);
                        f.tick(kT0 + 3000);
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
    checkRetiredNames();
    checkClock();
    checkThrottle();
    checkHeader();
    checkFenceFallback();
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
