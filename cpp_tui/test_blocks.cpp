// test_blocks.cpp
//
// The block renderer (views/blocks.h, design §4.5 / task 26): one v4 message
// drawn as a block, in both profiles' vocabulary.
//
// What this binary proves, in order:
//
//   1. the source. `views/blocks.cpp` writes no colour literal, never asks which
//      profile is up (`tuiIsTextGrid`), reads no display size and names no v4 type -
//      the layering every `views/` file is held to (`tools/gate_ratchet.txt`);
//   2. the vocabulary. Every kind's label, role ink, state glyph and anatomy are
//      pinned: a tool row is two lines, a work group and a thinking body are
//      disclosures, everything else is a role rule;
//   3. the timing. `blockTimingLabel()` renders every tier of the store's own
//      duration spellings, and a non-number is no duration at all;
//   4. the header. Label + duration + step count + (collapsed) preview, with the
//      grid's ASCII separator instead of the middle dot;
//   5. the fence/copy helpers in the markdown wrapper: a body that is one fenced code
//      block answers its *code*, and everything else answers nothing - so a copy can
//      never paste fences or half a document;
//   6. the anatomy, drawn. Each kind is rendered into a real 100x30 cell grid and read
//      back: the rule is one cell, the label and the state glyph are where the row
//      says they are, a compact row's summary is on its second line, a collapsed block
//      shows its preview and an open one does not, and the focus state is visible in
//      the cell ink (the GUI's ring is `test_gui_window`'s);
//   7. the real thread. A `TuiState` with real messages goes through
//      `views::renderThread()`: every message type draws, the tool state arrives as a
//      glyph *and* a word, and **no emoji reaches the screen** - the store writes
//      "✅"/"❌" into its summaries, and the adapter turns them into the row's state;
//   8. the focus and the copy. A click focuses exactly one block, the focused block
//      shows its Copy control, clicking it copies the body - and `y` (a character, the
//      TUI's keyboard path) copies the focused *code* block's code;
//
//   8b. copy without `Ctrl+C` (task 44). Every block type with content copies through
//      both doors - `y` on the focused row and the block menu on an unfocused one -
//      with the payload the type promises (code without fences, a table as the model's
//      markdown), while a body-less header is focusable and copies nothing. `Ctrl+C`
//      stays unbound (it is the terminal's `SIGINT`), so the quit path is unchanged;
//   9. the goldens. One 100x30 screen per message type, plus an ASCII and a 16-colour
//      variant, each pinning the glyphs *and* the inks and fills.
//
// The grid is the real text backend (the `TScreen` pattern of
// `test_session_filter_smoke.cpp`), so a golden is what the terminal would show, not
// what the drawing code believes it drew.
//
// Exit codes: 0 = all checks pass; 1 = a check failed; 2 = usage error.
//
//   ./test_blocks [-v] [--update]
//
// `--update` rewrites the fixtures; do that only for a deliberate rendering change, and
// commit the diff with it. They are captured with `LLMFUN_ASCII=0` and the true-colour
// theme, so a CJK locale, `TERM=linux` or a `COLORTERM` cannot change a golden.

#include "model/block.h"
#include "model/keymap.h"
#include "model/ui_state.h"
#include "store/thread_store.h"
#include "tui.h"
#include "ui/icons.h"
#include "ui/input.h"
#include "ui/layout.h"
#include "ui/markdown.h"
#include "ui/metrics.h"
#include "ui/tokens.h"
#include "views/blocks.h"
#include "views/thread.h"

#include "imtui/imtui.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace ui = llmfun::tui::ui;
namespace views = llmfun::tui::views;
namespace model = llmfun::tui::model;
namespace store = llmfun::tui::store;
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

    /// The row's text starting at `x`, `cells` wide - how a label is read back out of
    /// the grid instead of being compared with the string the drawing code used.
    std::string span(int x, int y, int cells) const {
        std::string text;
        for (int i = 0; i < cells; ++i) {
            const uint32_t cp = at(x + i, y);
            text += cp == 0 ? std::string(" ") : utf8Of(cp);
        }
        return text;
    }

    /// Whether the whole screen contains `needle`, in any row.
    bool contains(const std::string& needle) const {
        for (int y = 0; y < ny; ++y)
            if (rowText(y).find(needle) != std::string::npos)
                return true;
        return false;
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

/// One frame of the text grid: a decoration-free root window filling the display, the
/// body drawing inside it, then the frame rendered through the real backend.
///
/// `inject` runs *before* `NewFrame`, because that is when ImGui consumes its input
/// queue - an event added after it would only be seen by the next frame.
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
    ImGui::Begin("blocks-frame", nullptr,
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

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

model::BlockDesc blockOf(model::BlockKind kind, const char* label, const char* summary) {
    model::BlockDesc block;
    block.kind = kind;
    block.key = 7;
    block.label = label;
    block.summary = summary;
    block.expandable = true;
    block.expanded = true;
    block.hasBody = true;
    return block;
}

/// The header text for a descriptor whose `expanded` flag is the draw state. The
/// renderer takes a `BlockDraw` so the preview and the geometry cannot disagree;
/// the pure tests name the same state explicitly instead of relying on a second
/// read of the descriptor.
std::string headerText(const views::BlockView& block, bool textGrid) {
    return views::blockHeaderText(block, views::BlockDraw{block.expanded, false}, textGrid);
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

/// One message of the v4 store. The summary is the D side's own shape
/// (`"[<nr>] <body> [<time>]"`, `tui_chat.cpp`'s `addChatMessage` + `timeSuffix`), so
/// the adapter's splitter is exercised on what the app really writes.
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

/// A `TuiState` with a bound adapter - the state `views::renderThread()` is written
/// for.
struct ThreadFixture {
    tui::TuiState state;

    ThreadFixture() {
        state.readyStatus = true;
        state.ui.view = model::ThreadView::Ready;
        state.threadAdapter.bind(state);
    }

    void add(const tui::ChatMessage& msg) {
        state.chat.outputLines.push_back(msg);
        state.chat.hasStreamMsg = false;
    }

    /// The in-flight message: `chat.streamMsg` plus the flag the adapter's `sync()` reads
    /// (it is not in the store, so it has no id).
    void addStream(tui::ChatMessageType type, const std::string& text,
                   const std::string& thinking) {
        state.chat.streamMsg = message(0, type, "", text, thinking);
        state.chat.hasStreamMsg = true;
    }

    /// Draw the thread into the 100x30 frame and answer the captured grid.
    Grid draw(bool acceptKeys = true, const std::function<void(ImGuiIO&)>& inject = nullptr) {
        state.threadAdapter.bind(state);
        const ui::Rect region = cellRect(0.0f, 0.0f, float(kScreenW), float(kScreenH) - 1.0f);
        const ui::Rect earlier = cellRect(0.0f, 0.0f, 0.0f, 0.0f);
        return frame(
            [&](const ui::Metrics& m) {
                views::renderThread(state.ui, state.threadAdapter, region, earlier, m, 40, false,
                                    acceptKeys);
            },
            inject);
    }
};

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
    const char* path = TEST_BLOCKS_SOURCE;
    std::ifstream in(path);
    check(in.good(), std::string("views/blocks.cpp is readable (") + path + ")");
    if (!in.good())
        return;
    std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    check(source.find("tuiIsTextGrid") == std::string::npos,
          "the block renderer never asks which profile is up (the metrics carry it)");
    check(source.find("ImVec4(") == std::string::npos,
          "no colour literal: every ink is a token through ui::color()");
    check(source.find("ChatMessage") == std::string::npos &&
              source.find("RenderGroup") == std::string::npos,
          "no v4 type reaches the renderer (D-ADAPTER)");
    check(source.find("DisplaySize") == std::string::npos,
          "no display size read: the renderer is a geometry taker");
    check(source.find("#include \"tui.h\"") == std::string::npos,
          "no tui.h: the renderer needs a block, a rect and the metrics");
    check(source.find("GetIO()") == std::string::npos ||
              countOccurrences(source, "GetIO().InputQueueCharacters") == 0,
          "the renderer reads no input queue (the thread owns the `y` key)");
}

// ---------------------------------------------------------------------------
// 2. The vocabulary
// ---------------------------------------------------------------------------

void checkVocabulary() {
    struct Row {
        model::BlockKind kind;
        const char* label;
        ui::ColorToken ink;
        bool compact;
        bool disclosure;
    };
    const Row rows[] = {
        {model::BlockKind::User, "YOU", ui::ColorToken::roleYou, false, false},
        {model::BlockKind::Answer, "ANSWER", ui::ColorToken::roleAgent, false, false},
        {model::BlockKind::WorkGroup, "WORK", ui::ColorToken::roleAgent, false, true},
        {model::BlockKind::Assistant, "AGENT", ui::ColorToken::roleAgent, false, false},
        {model::BlockKind::ToolCall, "TOOL", ui::ColorToken::roleTool, true, false},
        {model::BlockKind::ToolResult, "TOOL", ui::ColorToken::roleTool, true, false},
        {model::BlockKind::Thinking, "Thinking", ui::ColorToken::roleAgent, false, true},
        {model::BlockKind::System, "SYSTEM", ui::ColorToken::roleSystem, false, false},
        {model::BlockKind::Code, "AGENT", ui::ColorToken::roleAgent, false, false},
    };
    for (const Row& row : rows) {
        check(std::strcmp(views::defaultBlockLabel(row.kind), row.label) == 0,
              std::string("the ") + row.label + " label");
        check(views::blockInk(row.kind) == row.ink,
              std::string(row.label) + " inks with its role token");
        check(views::blockIsCompactRow(row.kind) == row.compact,
              std::string(row.label) +
                  (row.compact ? " is a compact row" : " is not a compact row"));
        check(views::blockHasDisclosure(row.kind) == row.disclosure,
              std::string(row.label) + (row.disclosure ? " is a disclosure" : " is a role rule"));
    }

    check(views::blockStateGlyph(model::BlockState::None) == ui::Icon::Count,
          "no state means no glyph (the word carries the meaning)");
    check(views::blockStateGlyph(model::BlockState::Pending) == ui::Icon::Pending,
          "pending draws the pending mark");
    check(views::blockStateGlyph(model::BlockState::Running) == ui::Icon::Running,
          "running draws the run mark");
    check(views::blockStateGlyph(model::BlockState::Ok) == ui::Icon::Ok, "ok draws the ok mark");
    check(views::blockStateGlyph(model::BlockState::Failed) == ui::Icon::Error,
          "failed draws the error mark");

    const ui::Metrics m = ui::metricsFor(true, 1.0f, 1.0f);
    const views::BlockDraw open{true, false};
    const views::BlockDraw closed{false, false};
    check(views::measureBlockHeader(blockOf(model::BlockKind::User, "YOU", "q"), m, open) == m.row,
          "a role row is one row tall");
    const model::BlockDesc tool = blockOf(model::BlockKind::ToolResult, "TOOL", "r");
    check(views::measureBlockHeader(tool, m, closed) == 2.0f * m.row,
          "a compact tool row is two rows tall while it is closed");
    check(views::measureBlockHeader(tool, m, open) == m.row,
          "an open tool row gives its summary line back (the body carries the text)");
    check(views::measureBlockHeader(blockOf(model::BlockKind::WorkGroup, "WORK", ""), m, open) ==
              m.row,
          "a group header is one row tall");
    // The expansion is an argument, not a second read of the descriptor: a caller whose
    // draw state disagrees with the descriptor still gets the height it will draw.
    model::BlockDesc stale = tool;
    stale.expanded = true;
    check(views::measureBlockHeader(stale, m, closed) == 2.0f * m.row,
          "the measurement follows the draw state it is handed");
}

// ---------------------------------------------------------------------------
// 3. The timing
// ---------------------------------------------------------------------------

void checkTiming() {
    check(views::blockTimingLabel(0.0).empty(), "no duration, no suffix");
    check(views::blockTimingLabel(-1.0).empty(), "a negative duration is no duration");
    check(views::blockTimingLabel(std::nan("")).empty(), "a non-number is no duration");
    check(views::blockTimingLabel(0.32) == "320 ms",
          "under a second is milliseconds (got " + views::blockTimingLabel(0.32) + ")");
    check(views::blockTimingLabel(3.2) == "3.2 s",
          "seconds keep one decimal (got " + views::blockTimingLabel(3.2) + ")");
    check(views::blockTimingLabel(65.0) == "1m 5s",
          "a minute is minutes and seconds (got " + views::blockTimingLabel(65.0) + ")");
    check(views::blockTimingLabel(3725.0) == "1h 2m",
          "an hour is hours and minutes (got " + views::blockTimingLabel(3725.0) + ")");
}

// ---------------------------------------------------------------------------
// 4. The header line
// ---------------------------------------------------------------------------

void checkHeaderText() {
    model::BlockDesc user = blockOf(model::BlockKind::User, "YOU", "a question");
    user.expanded = false;
    check(headerText(user, true) == "YOU  a question",
          "a closed block says its preview (got '" + headerText(user, true) + "')");
    user.expanded = true;
    check(headerText(user, true) == "YOU",
          "an open block is a pure role row (got '" + headerText(user, true) + "')");
    // The draw state is the authority: a descriptor whose `expanded` disagrees with
    // the frame still previews per the frame, matching the height and the disclosure.
    model::BlockDesc mismatched = user;
    mismatched.expanded = true;
    check(views::blockHeaderText(mismatched, views::BlockDraw{false, false}, true) ==
              "YOU  a question",
          "the preview follows the draw state, not the descriptor");
    user.timingS = 1.2;
    check(headerText(user, true) == "YOU - 1.2 s",
          "the grid separates with ASCII (got '" + headerText(user, true) + "')");
    check(headerText(user, false) == "YOU \xC2\xB7 1.2 s",
          "the pixel profile separates with the middle dot");

    model::BlockDesc group = blockOf(model::BlockKind::WorkGroup, "WORK", "");
    group.steps = 4;
    group.expandable = true;
    check(headerText(group, false) == "WORK \xC2\xB7 4 steps",
          "the work group says how many steps it holds (got '" + headerText(group, false) + "')");
    group.steps = 1;
    check(headerText(group, true) == "WORK - 1 step", "one step is singular");

    model::BlockDesc tool = blockOf(model::BlockKind::ToolResult, "TOOL", "writeFile: 42 lines");
    tool.state = model::BlockState::Ok;
    check(headerText(tool, true) == "TOOL",
          "a tool row's first line is its label (got '" + headerText(tool, true) + "')");
    check(views::blockRowText(tool) == "writeFile: 42 lines",
          "a tool row's second line is its summary");
    check(views::blockRowText(user).empty(), "only a compact row has a second line");

    model::BlockDesc thinking = blockOf(model::BlockKind::Thinking, "Thinking", "");
    thinking.timingS = 3.2;
    thinking.expanded = false;
    check(headerText(thinking, true) == "Thinking - 3.2 s",
          "the thinking disclosure says what it costs (got '" + headerText(thinking, true) + "')");

    model::BlockDesc stream = blockOf(model::BlockKind::Assistant, "AGENT", "half a senten");
    stream.streaming = true;
    stream.expanded = true;
    check(headerText(stream, true) == "AGENT",
          "a streaming row carries no preview: its body is being written under it");

    // Whatever a label says, it is never an emoji (task 26's rule, at the last place
    // the text is chosen).
    const model::BlockKind kinds[] = {
        model::BlockKind::User,      model::BlockKind::Answer,   model::BlockKind::WorkGroup,
        model::BlockKind::Assistant, model::BlockKind::ToolCall, model::BlockKind::ToolResult,
        model::BlockKind::Thinking,  model::BlockKind::System,   model::BlockKind::Code};
    for (const model::BlockKind kind : kinds) {
        const std::string label = views::defaultBlockLabel(kind);
        bool ascii = !label.empty();
        for (const char c : label)
            ascii = ascii && static_cast<unsigned char>(c) < 0x80;
        check(ascii, "the label '" + label + "' is plain ASCII");
    }
}

// ---------------------------------------------------------------------------
// 5. The fence/copy helpers
// ---------------------------------------------------------------------------

void checkCopyHelpers() {
    check(ui::fencedCodeOf("```\nint x = 1;\n```") == "int x = 1;\n",
          "one fenced block answers its code");
    check(ui::fencedCodeOf("```cpp\nint x = 1;\n```") == "int x = 1;\n",
          "the info string is not part of the code");
    check(ui::fencedCodeOf("\n\n```\ncode\n```\n\n") == "code\n",
          "blank lines around the fence are not part of it");
    check(ui::fencedCodeOf("```\r\ncode\r\n```\r\n") == "code\n",
          "a CRLF body answers LF code (the CR was the line discipline's)");
    check(ui::fencedCodeOf("~~~\ncode\n~~~") == "code\n", "a tilde fence is a fence");
    check(ui::fencedCodeOf("prose\n```\ncode\n```").empty(),
          "prose before a fence is not a code block");
    check(ui::fencedCodeOf("```\na\n```\n```\nb\n```").empty(),
          "two code blocks are not one code block");
    check(ui::fencedCodeOf("```\nunterminated\n").empty(), "an unterminated fence answers nothing");
    check(ui::fencedCodeOf("just prose").empty(), "prose is not code");
    check(ui::fencedCodeOf("").empty(), "an empty body is not code");
    check(ui::fencedCodeOf("```\na\n~~~\n```") == "a\n~~~\n",
          "a different marker inside the block does not close it");

    check(ui::copyText("```\ncode\n```", true) == "code\n", "a code block copies its code");
    check(ui::copyText("plain text", false) == "plain text", "a body copies verbatim");
    check(ui::copyText("not a fence", true) == "not a fence",
          "a code block whose fences do not parse falls back to the body (never an empty paste)");
}

// ---------------------------------------------------------------------------
// 6. The anatomy, drawn
// ---------------------------------------------------------------------------

void checkAnatomy() {
    // A role row: the rule is one cell, the label starts right after it.
    model::BlockDesc user = blockOf(model::BlockKind::User, "YOU", "a question");
    user.expanded = false;
    Grid grid = frame([&](const ui::Metrics& m) {
        const ui::Rect rect = cellRect(0.0f, 0.0f, 40.0f, m.row);
        views::renderBlock(user, rect, m, views::BlockDraw{false, false});
    });
    check(grid.at(0, 0) != 0, "the role rule paints the first cell");
    check(grid.bgAt(0, 0) != 0, "the rule is a fill in the role's own ink");
    check(grid.span(1, 0, 3) == "YOU",
          "the label follows the rule (got '" + grid.span(1, 0, 3) + "')");
    check(grid.rowText(0).find("a question") != std::string::npos,
          "a closed row shows its preview");
    // The role is the *foreground*: two roles must not ink the same, or the label stops
    // saying who is speaking (D-BLOCK: role via foreground + rule).
    const uint8_t userInk = grid.fgAt(1, 0);
    model::BlockDesc answer = blockOf(model::BlockKind::Answer, "ANSWER", "a result");
    Grid answerGrid = frame([&](const ui::Metrics& m) {
        const ui::Rect rect = cellRect(0.0f, 0.0f, 40.0f, m.row);
        views::renderBlock(answer, rect, m, views::BlockDraw{true, false});
    });
    check(answerGrid.fgAt(1, 0) != userInk, "each role inks with its own token");

    // The same row open: the preview is gone (the body speaks for itself).
    user.expanded = true;
    grid = frame([&](const ui::Metrics& m) {
        const ui::Rect rect = cellRect(0.0f, 0.0f, 40.0f, m.row);
        views::renderBlock(user, rect, m, views::BlockDraw{true, false});
    });
    check(grid.rowText(0).find("a question") == std::string::npos,
          "an open row carries no preview");

    // A compact row: two lines, the summary under the label.
    model::BlockDesc tool = blockOf(model::BlockKind::ToolResult, "TOOL", "writeFile: 42 lines");
    tool.state = model::BlockState::Ok;
    tool.expanded = false;
    grid = frame([&](const ui::Metrics& m) {
        const ui::Rect rect = cellRect(0.0f, 0.0f, 40.0f, 2.0f * m.row);
        views::renderBlock(tool, rect, m, views::BlockDraw{false, false});
    });
    check(grid.span(3, 0, 4) == "TOOL",
          "the tool label sits after the rule and the glyph (got '" + grid.span(3, 0, 4) + "')");
    check(grid.rowText(1).find("writeFile: 42 lines") != std::string::npos,
          "the summary is the row's second line");
    check(grid.at(1, 0) != 0, "the state glyph paints its slot");

    // A disclosure instead of a rule: the work group's mark toggles, so the first cell
    // is the closed mark, and opening it changes that cell.
    model::BlockDesc group = blockOf(model::BlockKind::WorkGroup, "WORK", "");
    group.steps = 4;
    Grid closed = frame([&](const ui::Metrics& m) {
        const ui::Rect rect = cellRect(0.0f, 0.0f, 40.0f, m.row);
        views::renderBlock(group, rect, m, views::BlockDraw{false, false});
    });
    Grid opened = frame([&](const ui::Metrics& m) {
        const ui::Rect rect = cellRect(0.0f, 0.0f, 40.0f, m.row);
        views::renderBlock(group, rect, m, views::BlockDraw{true, false});
    });
    check(closed.at(0, 0) != 0 && opened.at(0, 0) != 0, "the disclosure paints a mark");
    check(closed.at(0, 0) != opened.at(0, 0), "the mark says whether the row is open");
    check(closed.rowText(0).find("WORK") != std::string::npos, "the group row says WORK");
    check(closed.rowText(0).find("4 steps") != std::string::npos,
          "the group row says how many steps it holds");

    // Focus: the label lifts to the accent ink and the Copy control appears.
    Grid unfocused = frame([&](const ui::Metrics& m) {
        const ui::Rect rect = cellRect(0.0f, 0.0f, 40.0f, m.row);
        views::renderBlock(user, rect, m, views::BlockDraw{true, false});
    });
    Grid focused = frame([&](const ui::Metrics& m) {
        const ui::Rect rect = cellRect(0.0f, 0.0f, 40.0f, m.row);
        views::renderBlock(user, rect, m, views::BlockDraw{true, true});
    });
    check(focused.fgAt(1, 0) != unfocused.fgAt(1, 0),
          "the focused block's label uses the focus ink (the grid's visible focus state)");
    check(focused.rowText(0).find("Copy") != std::string::npos,
          "the focused block offers its Copy control");
    check(unfocused.rowText(0).find("Copy") == std::string::npos,
          "an unfocused block shows no Copy control");

    // The ring is the pixel profile's focus state, and it is drawn from the token that
    // means "focus" (accent at 60 %). The cell renderer cannot draw a ring - the design's
    // accepted hole - so this frame renders the *pixel* metrics through the text backend:
    // the ring's stroke lands as painted cells, which is enough to prove the branch draws
    // something (and only when the block is focused).
    auto paintedCells = [](const Grid& g) {
        int painted = 0;
        for (int y = 0; y < g.ny; ++y)
            for (int x = 0; x < g.nx; ++x)
                painted += g.bgAt(x, y) != 0 ? 1 : 0;
        return painted;
    };
    const ui::Metrics pixel = ui::metricsFor(false, 1.0f, 1.0f);
    const Grid plain = frame([&](const ui::Metrics&) {
        const ui::Rect rect{0.0f, 0.0f, 20.0f * pixel.gu, pixel.row, false};
        views::renderBlock(user, rect, pixel, views::BlockDraw{true, false});
    });
    const Grid ring = frame([&](const ui::Metrics&) {
        const ui::Rect rect{0.0f, 0.0f, 20.0f * pixel.gu, pixel.row, false};
        views::renderBlock(user, rect, pixel, views::BlockDraw{true, true});
    });
    const int plainCells = paintedCells(plain);
    const int ringCells = paintedCells(ring);
    check(ringCells > plainCells, "the pixel profile draws the focus ring (painted cells " +
                                      std::to_string(plainCells) + " -> " +
                                      std::to_string(ringCells) + ")");

    // The ring's *own* ink, counted in the grid instead of inferred from "some cell was painted":
    // the focused frame paints the `focusRing` token's palette entry, and a frame with no focused
    // block paints none of it. Without the second half the first one rots into a tautology (any
    // extra fill would satisfy it) - and the GUI capture cannot carry the pair any more, because
    // the workbench draws a ring for the composer as well (task-41 review, finding 3).
    const int ringIndex = ui::tokens().xterm256Index[int(ui::ColorToken::focusRing)];
    const auto inkCells = [ringIndex](const Grid& g) {
        int cells = 0;
        for (int y = 0; y < g.ny; ++y)
            for (int x = 0; x < g.nx; ++x)
                cells += g.bgAt(x, y) == ringIndex ? 1 : 0;
        return cells;
    };
    const int ringInk = inkCells(ring);
    const int plainInk = inkCells(plain);
    check(ringInk >= 4, "the focused block paints the ring token's own entry (" +
                            std::to_string(ringInk) + " cells, entry " + std::to_string(ringIndex) +
                            ")");
    check(plainInk == 0, "an unfocused block paints no ring ink (" + std::to_string(plainInk) +
                             " cells of entry " + std::to_string(ringIndex) + ")");
}

// ---------------------------------------------------------------------------
// 7. The real thread
// ---------------------------------------------------------------------------

void checkThreadDrawsEveryKind() {
    ThreadFixture fixture;
    fixture.add(message(1, tui::ChatMessageType::User, "[1] how do I build it [1.2s]",
                        "how do I build it"));
    fixture.add(message(2, tui::ChatMessageType::FinalAnswer, "[2] run make [3.4s]", "run make"));
    Grid grid = fixture.draw();
    check(grid.contains("YOU"), "a user query draws its role");
    check(grid.contains("ANSWER"), "the final answer draws its role");
    check(grid.contains("1.2 s"), "a message's duration reaches the row");
    check(grid.contains("how do I build it"), "a closed message previews its text");

    // A work group: header + one row per step, with the tool state as glyph + word.
    ThreadFixture work;
    work.add(message(1, tui::ChatMessageType::Assistant, "[1] thinking about it [1.0s]",
                     "I will list the files", "reasoning here"));
    work.add(message(2, tui::ChatMessageType::ToolCall, "[2] tool call: bash ls [1.1s]",
                     "tool call: bash ls"));
    work.add(message(3, tui::ChatMessageType::ToolResponse,
                     "[3] tool result \xE2\x9C\x85 writeFile: 42 lines [1.2s]",
                     "tool result writeFile: 42 lines"));
    work.add(
        message(4, tui::ChatMessageType::System, "[4] note: 42 files [1.3s]", "note: 42 files"));
    grid = work.draw();
    check(grid.contains("WORK"), "the work group draws its header");
    check(grid.contains("4 steps"), "the work group counts its steps");
    check(grid.contains("Thinking"), "the thinking disclosure has its own row");
    check(grid.contains("TOOL"), "a tool row draws its label");
    check(grid.contains("writeFile: 42 lines"), "a tool row draws its summary");
    check(grid.contains("SYSTEM"), "a system notice draws its label");

    // The store writes the outcome as an emoji; the row must not.
    for (int y = 0; y < grid.ny; ++y) {
        const std::string row = grid.rowText(y);
        bool emoji = false;
        for (std::size_t i = 0; i < row.size();) {
            const unsigned char c = static_cast<unsigned char>(row[i]);
            std::size_t len = 1;
            uint32_t cp = c;
            if ((c & 0xE0) == 0xC0) {
                len = 2;
                cp = c & 0x1F;
            } else if ((c & 0xF0) == 0xE0) {
                len = 3;
                cp = c & 0x0F;
            } else if ((c & 0xF8) == 0xF0) {
                len = 4;
                cp = c & 0x07;
            }
            for (std::size_t k = 1; k < len && i + k < row.size(); ++k)
                cp = (cp << 6) | (static_cast<unsigned char>(row[i + k]) & 0x3F);
            if (cp == 0x2705 || cp == 0x274C || (cp >= 0x1F300 && cp <= 0x1FAFF))
                emoji = true;
            i += len;
        }
        check(!emoji, "no emoji reaches the screen (row " + std::to_string(y) + ")");
    }

    // A code block: the body is one fence, so the block is a `Code` block and its copy
    // path copies the code.
    ThreadFixture code;
    code.add(message(1, tui::ChatMessageType::Assistant, "[1] here it is [1.0s]",
                     "```\nint main() { return 0; }\n```"));
    grid = code.draw();
    check(grid.contains("AGENT"), "a code body keeps the message's role word");
    check(grid.contains("int main"), "the code body is drawn");
}

// ---------------------------------------------------------------------------
// 8. The focus and the copy
// ---------------------------------------------------------------------------

/// A clipboard the test owns: the text backend's real one shells out to xclip, and a
/// test must not depend on a display server for its assertions.
std::string g_clipboard;
bool g_clipboardSet = false;

void installClipboard() {
    ImGuiIO& io = ImGui::GetIO();
    io.ClipboardUserData = nullptr;
    io.SetClipboardTextFn = [](void*, const char* text) {
        g_clipboard = text != nullptr ? text : "";
        g_clipboardSet = true;
    };
    io.GetClipboardTextFn = [](void*) { return g_clipboard.c_str(); };
}

// ---------------------------------------------------------------------------
// 7b. The streaming message
// ---------------------------------------------------------------------------

/// The streamed message is not in the store, and its reasoning is the *first* thing a
/// run produces. HEAD drew it inside the streamed body (`renderMessageBody(..., true)`);
/// the block restyle gives it a row of its own - and this phase is the one that would
/// have caught drawing an empty `AGENT` row for a reasoning-only stream.
void checkStreaming() {
    installClipboard();

    ThreadFixture reasoning;
    reasoning.addStream(tui::ChatMessageType::Assistant, "", "REASONING-TEXT-VISIBLE");
    Grid grid = reasoning.draw();
    check(grid.contains("AGENT"), "a streaming message draws its role");
    check(grid.contains("Thinking"), "a reasoning-only stream draws the thinking row");
    check(grid.contains("REASONING-TEXT-VISIBLE"), "and the reasoning itself (got a blank row?)");

    ThreadFixture both;
    both.addStream(tui::ChatMessageType::Assistant, "STREAM-PROSE", "STREAM-THINKING");
    grid = both.draw();
    check(grid.contains("STREAM-THINKING"), "a stream with both draws the reasoning");
    check(grid.contains("STREAM-PROSE"), "and the prose");
    check(grid.rowText(0).find("STREAM-PROSE") == std::string::npos,
          "the prose is not on the reasoning row");
    check(both.state.threadAdapter.blockExpanded(0, 0), "the streamed reasoning starts open");

    // The rows are one cell tall each: block 0 is the disclosure, block 1 the body.
    auto clickRow = [&](float y) {
        both.draw(true, [y](ImGuiIO& in) { in.AddMousePosEvent(3.0f, y); });
        both.draw(true, [](ImGuiIO& in) { in.AddMouseButtonEvent(0, true); });
        both.draw(true, [](ImGuiIO& in) { in.AddMouseButtonEvent(0, false); });
        return both.draw(true); // the click's answer is drawn on the next frame
    };

    // The reasoning row is a disclosure like any other - and a copy target of its own.
    grid = clickRow(0.0f);
    check(both.state.ui.thread.focus ==
              model::BlockId{1, model::kStreamMessageId, model::BlockKind::Thinking},
          "the reasoning row is focusable (the stream's BlockId)");
    check(!both.state.threadAdapter.blockExpanded(0, 0), "a click on it closes it");
    check(!grid.contains("STREAM-THINKING"), "a closed reasoning row hides its text");
    check(grid.contains("STREAM-PROSE"), "the prose stays");

    grid = clickRow(0.0f);
    check(both.state.threadAdapter.blockExpanded(0, 0), "a second click opens it again");
    check(grid.contains("STREAM-THINKING"), "and the reasoning is back");
    g_clipboard.clear();
    g_clipboardSet = false;
    both.draw(true, [](ImGuiIO& in) { in.AddInputCharacter('y'); });
    check(g_clipboard == "STREAM-THINKING",
          "`y` on the reasoning copies the reasoning (got '" + g_clipboard + "')");

    // The body row is its own target: `y` there copies the prose. Its row is *below* the
    // reasoning body, so it is found in the captured grid rather than assumed.
    int agentRow = -1;
    for (int y = 0; y < grid.ny; ++y)
        if (grid.rowText(y).find("AGENT") != std::string::npos && agentRow < 0)
            agentRow = y;
    check(agentRow > 0,
          "the body row is below the reasoning row (row " + std::to_string(agentRow) + ")");
    clickRow(static_cast<float>(agentRow));
    check(both.state.ui.thread.focus ==
              model::BlockId{1, model::kStreamMessageId, model::BlockKind::Assistant},
          "the body row is focusable too");
    g_clipboard.clear();
    g_clipboardSet = false;
    both.draw(true, [](ImGuiIO& in) { in.AddInputCharacter('y'); });
    check(g_clipboard == "STREAM-PROSE",
          "`y` on the body copies the prose (got '" + g_clipboard + "')");
}

void checkFocusAndCopy() {
    installClipboard();

    // Two messages so "exactly one block is focused" means something.
    ThreadFixture fixture;
    fixture.add(
        message(1, tui::ChatMessageType::User, "[1] first question [0.5s]", "first question"));
    fixture.add(
        message(2, tui::ChatMessageType::User, "[2] second question [0.6s]", "second question"));
    fixture.state.ui.view = model::ThreadView::Ready;
    fixture.draw();

    // Click the first row: it takes the focus, and the second block does not.
    fixture.draw(true, [](ImGuiIO& in) { in.AddMousePosEvent(5.0f, 0.0f); });
    fixture.draw(true, [](ImGuiIO& in) { in.AddMouseButtonEvent(0, true); });
    fixture.draw(true, [](ImGuiIO& in) { in.AddMouseButtonEvent(0, false); });
    // The chip is drawn by the frame *after* the one that took the focus: the click is
    // reported by the row, and the row that reported it was already painted.
    Grid grid = fixture.draw(true);
    // One block at a time: the state is a single BlockId, and the click that took it
    // names the first block (the user query's group, message id and kind).
    check(fixture.state.ui.thread.focus == model::BlockId{1, 1, model::BlockKind::User},
          "a click focuses a block");
    check(grid.contains("Copy"), "the focused block shows its Copy control");

    const std::string body = fixture.state.threadAdapter.blockText(0, 0);
    check(body == "first question", "the adapter answers the block's body (got '" + body + "')");

    // Exactly one block is focused: one row carries the control, however many blocks the
    // thread holds (the focus is a single BlockId, so this is the drawing's half of that
    // rule).
    int copyRows = 0;
    for (int y = 0; y < grid.ny; ++y)
        copyRows += grid.rowText(y).find("Copy") != std::string::npos ? 1 : 0;
    check(copyRows == 1, "exactly one row carries the Copy control (one focused block)");
    check(grid.rowText(0).find("Copy") != std::string::npos,
          "the Copy control is drawn on the focused row");

    // The mouse path (task 26: "copy works by mouse"): the row's hit box gives up exactly
    // the chip's width, so a click on the chip's own cells copies the block - and does
    // not reach the row underneath it (no toggle, no focus change).
    const std::size_t chipAt = grid.rowText(0).find("Copy");
    const float chipX = static_cast<float>(chipAt) + 2.0f;
    const bool wasExpanded = fixture.state.threadAdapter.blockExpanded(0, 0);
    g_clipboard.clear();
    g_clipboardSet = false;
    fixture.draw(true, [chipX](ImGuiIO& in) { in.AddMousePosEvent(chipX, 0.0f); });
    fixture.draw(true, [](ImGuiIO& in) { in.AddMouseButtonEvent(0, true); });
    fixture.draw(true, [](ImGuiIO& in) { in.AddMouseButtonEvent(0, false); });
    check(g_clipboardSet && g_clipboard == "first question",
          "a click on the Copy control copies the block (got '" + g_clipboard + "')");
    check(fixture.state.ui.thread.focus == model::BlockId{1, 1, model::BlockKind::User},
          "the chip click keeps the focus where it was");

    // `y` copies the focused block's body - a character, because the terminal never
    // delivers a printable key as a key event (plan/key_gate.md F2).
    g_clipboard.clear();
    g_clipboardSet = false;
    fixture.draw(true, [](ImGuiIO& in) { in.AddInputCharacter('y'); });
    check(g_clipboardSet, "`y` asks for the clipboard");
    check(g_clipboard == "first question",
          "`y` copies the focused block (got '" + g_clipboard + "')");

    // A block with no focus copies nothing: one `y`, one target.
    ThreadFixture fresh;
    fresh.add(
        message(1, tui::ChatMessageType::User, "[1] nobody focused [0.5s]", "nobody focused"));
    fresh.draw();
    g_clipboard.clear();
    g_clipboardSet = false;
    fresh.draw(true, [](ImGuiIO& in) { in.AddInputCharacter('y'); });
    check(!g_clipboardSet, "with no focused block, `y` copies nothing");

    // And a *code* block's `y` copies the code, not the fences (S-13: the TUI path).
    ThreadFixture code;
    code.add(
        message(1, tui::ChatMessageType::Assistant, "[1] here [1.0s]", "```\nint x = 1;\n```"));
    code.draw();
    // The code block is the *second* row of its work group: the group's header is the
    // first, so the focus has to be put on the row that holds the body.
    code.draw(true, [](ImGuiIO& in) { in.AddMousePosEvent(2.0f, 1.0f); });
    code.draw(true, [](ImGuiIO& in) { in.AddMouseButtonEvent(0, true); });
    code.draw(true, [](ImGuiIO& in) { in.AddMouseButtonEvent(0, false); });
    code.draw(true); // the click's answer is drawn on the next frame
    g_clipboard.clear();
    g_clipboardSet = false;
    code.draw(true, [](ImGuiIO& in) { in.AddInputCharacter('y'); });
    check(g_clipboard == "int x = 1;\n",
          "`y` on a focused code block copies the code (got '" + g_clipboard + "')");

    // A text field keeps its characters: the guard is `ui::takeCharacter`'s own
    // `WantTextInput` check (pinned by the rail's key tests, its first consumer). What
    // this binary can pin without a text widget is the half that matters here - the
    // queue is consumed *by value*, so the renderer can neither see a stray character
    // nor take one it did not ask for.
    grid = fixture.draw();
    check(!ui::takeCharacter('y'), "an empty queue consumes nothing");
    // The queue a backend fills (`InputQueueCharacters`), filled directly: an *event*
    // only reaches it on the next `NewFrame`, and this is a unit check of the helper.
    ImGui::GetIO().InputQueueCharacters.push_back('y');
    check(ui::takeCharacter('z') == false, "a different character is left in the queue");
    check(ui::takeCharacter('y'), "the queued character is the one that is taken");
    check(!ui::takeCharacter('y'), "and it is taken exactly once");
}

/// The streamed reasoning starts open for every stream, not just the first one of
/// the process: the state is seeded when the stream group is born, and a clear must
/// give the next stream a new group id so the seed is not inherited.
/// The copy's answer (task 45, design §5.25 / FR-29): every copy path raises *one* feedback item
/// naming the block it copied, and a row that copies nothing raises none - the body-less row is
/// the case the task names, and a "Copied WORK" for a block with no body would be a lie the user
/// cannot check.

void checkStreamReasoningDefault() {
    ThreadFixture fixture;
    fixture.state.ui.view = model::ThreadView::Ready;
    tui::tuiUpdateStreamChatMessage(
        fixture.state, message(0, tui::ChatMessageType::Assistant, "", "prose", "reasoning"));
    fixture.draw();
    check(fixture.state.threadAdapter.blockExpanded(0, 0), "a new stream's reasoning starts open");

    // Close it, as a click on the disclosure would.
    fixture.state.threadAdapter.setBlockExpanded(0, 0, false);
    check(!fixture.state.threadAdapter.blockExpanded(0, 0), "the streamed reasoning can be closed");

    // A focus on a streamed row names a block that disappears with the stream; the
    // clear must drop it, or `y` would keep consuming characters for a dead target.
    fixture.state.ui.thread.focus =
        model::BlockId{1, model::kStreamMessageId, model::BlockKind::Thinking};
    // Commit the stream and start the next one: the new group starts open.
    tui::tuiStreamChatMessageClear(fixture.state);
    check(!fixture.state.ui.thread.focus.valid(), "clearing the stream clears a focus on its rows");
    tui::tuiUpdateStreamChatMessage(
        fixture.state, message(0, tui::ChatMessageType::Assistant, "", "prose2", "reasoning2"));
    fixture.draw();
    check(fixture.state.threadAdapter.blockExpanded(0, 0),
          "a later stream's reasoning starts open (the flag is not sticky)");
}

/// A stored work group and a stream in one thread: the shape where the two id
/// namespaces meet. The stream group is 3 (user = 1, work = 2), and message 3 has
/// both an Assistant body and a Thinking disclosure, so a `(key, kind)` identity
/// would match two rows for each kind. The BlockId (group, key, kind) keeps the
/// stored and streamed rows apart, and the stream's reserved key 0 is focusable
/// because its group is not 0.
void checkCrossGroupIdentity() {
    installClipboard();

    ThreadFixture fixture;
    fixture.add(message(1, tui::ChatMessageType::User, "[1] question [0.5s]", "question"));
    fixture.add(message(2, tui::ChatMessageType::Assistant, "[2] stored two [1.0s]", "stored-two"));
    fixture.add(message(3, tui::ChatMessageType::Assistant, "[3] stored three [2.0s]",
                        "stored-three", "think-three"));
    fixture.state.ui.view = model::ThreadView::Ready;
    fixture.addStream(tui::ChatMessageType::Assistant, "stream-prose", "stream-reasoning");
    Grid grid = fixture.draw();

    // The invariant first: this is the assertion that names the cause when a future
    // adapter lets two id namespaces meet.
    std::set<model::BlockId> seen;
    for (std::size_t g = 0; g < fixture.state.threadAdapter.groupCount(); ++g) {
        const store::GroupView group = fixture.state.threadAdapter.groupAt(g);
        for (std::size_t b = 0; b < fixture.state.threadAdapter.blockCount(g); ++b) {
            const model::BlockDesc desc = fixture.state.threadAdapter.blockAt(g, b);
            check(seen.insert(desc.id()).second,
                  "block identities are unique (group " + std::to_string(desc.group) + ", key " +
                      std::to_string(desc.key) + ", kind " +
                      std::to_string(static_cast<int>(desc.kind)) + ")");
        }
    }

    const auto rowOf = [](const Grid& g, const std::string& needle, bool fromBottom = false) {
        if (fromBottom) {
            for (int y = g.ny - 1; y >= 0; --y)
                if (g.rowText(y).find(needle) != std::string::npos)
                    return y;
        } else {
            for (int y = 0; y < g.ny; ++y)
                if (g.rowText(y).find(needle) != std::string::npos)
                    return y;
        }
        return -1;
    };
    const auto clickRow = [&](int y) {
        fixture.draw(true, [y](ImGuiIO& in) { in.AddMousePosEvent(5.0f, static_cast<float>(y)); });
        fixture.draw(true, [](ImGuiIO& in) { in.AddMouseButtonEvent(0, true); });
        fixture.draw(true, [](ImGuiIO& in) { in.AddMouseButtonEvent(0, false); });
        return fixture.draw(true);
    };
    const auto copyRows = [](const Grid& g) {
        int count = 0;
        for (int y = 0; y < g.ny; ++y)
            count += g.rowText(y).find("Copy") != std::string::npos ? 1 : 0;
        return count;
    };

    // The stored Assistant body: group 2, key 3. Its 2.0 s timing is unique here.
    const int storedBody = rowOf(grid, "AGENT - 2.0 s");
    check(storedBody >= 0, "the stored message 3 body row is on screen");
    Grid afterStoredBody = clickRow(storedBody);
    check(fixture.state.ui.thread.focus == model::BlockId{2, 3, model::BlockKind::Assistant},
          "the stored body focuses its own group and message");
    check(copyRows(afterStoredBody) == 1,
          "focusing the stored body shows exactly one Copy control");
    g_clipboard.clear();
    g_clipboardSet = false;
    fixture.draw(true, [](ImGuiIO& in) { in.AddInputCharacter('y'); });
    check(g_clipboard == "stored-three",
          "`y` on the stored body copies the stored text (got '" + g_clipboard + "')");

    // The stored Thinking row: same group and key, a different kind - a separate
    // identity and a separate copy target.
    const int storedThinking = rowOf(afterStoredBody, "Thinking - 2.0 s");
    check(storedThinking >= 0, "the stored message 3 thinking row is on screen");
    Grid afterStoredThinking = clickRow(storedThinking);
    check(fixture.state.ui.thread.focus == model::BlockId{2, 3, model::BlockKind::Thinking},
          "the stored thinking focuses the Thinking kind");
    check(copyRows(afterStoredThinking) == 1,
          "focusing the stored thinking shows exactly one Copy control");
    g_clipboard.clear();
    g_clipboardSet = false;
    fixture.draw(true, [](ImGuiIO& in) { in.AddInputCharacter('y'); });
    check(g_clipboard == "think-three",
          "`y` on the stored thinking copies the stored reasoning (got '" + g_clipboard + "')");

    // The stream's rows are the other half of each pair. The stream body is the last
    // AGENT header (it has no timing); the stream thinking is the last Thinking one.
    const int streamBody = rowOf(afterStoredThinking, "AGENT", /*fromBottom=*/true);
    check(streamBody >= 0, "the streamed body row is on screen");
    Grid afterStreamBody = clickRow(streamBody);
    check(fixture.state.ui.thread.focus ==
              model::BlockId{3, model::kStreamMessageId, model::BlockKind::Assistant},
          "the streamed body is focusable with the reserved key 0");
    check(copyRows(afterStreamBody) == 1, "the streamed body is the only focused row");
    g_clipboard.clear();
    g_clipboardSet = false;
    fixture.draw(true, [](ImGuiIO& in) { in.AddInputCharacter('y'); });
    check(g_clipboard == "stream-prose",
          "`y` on the streamed body copies the stream (got '" + g_clipboard + "')");

    const int streamThinking = rowOf(afterStreamBody, "Thinking", /*fromBottom=*/true);
    check(streamThinking >= 0, "the streamed reasoning row is on screen");
    Grid afterStreamThinking = clickRow(streamThinking);
    check(fixture.state.ui.thread.focus ==
              model::BlockId{3, model::kStreamMessageId, model::BlockKind::Thinking},
          "the streamed reasoning is focusable with the reserved key 0");
    check(copyRows(afterStreamThinking) == 1, "the streamed reasoning is the only focused row");
    g_clipboard.clear();
    g_clipboardSet = false;
    fixture.draw(true, [](ImGuiIO& in) { in.AddInputCharacter('y'); });
    check(g_clipboard == "stream-reasoning",
          "`y` on the streamed reasoning copies it (got '" + g_clipboard + "')");
}
// ---------------------------------------------------------------------------
// 8b. The block menu (task 44) and the blocks with nothing to copy
// ---------------------------------------------------------------------------

/// The first row whose text contains `needle` (the last one when `fromBottom`), or -1.
/// Rows are read back from the captured grid, never assumed from the layout.
int findRow(const Grid& grid, const std::string& needle, bool fromBottom = false) {
    if (fromBottom) {
        for (int y = grid.ny - 1; y >= 0; --y)
            if (grid.rowText(y).find(needle) != std::string::npos)
                return y;
    } else {
        for (int y = 0; y < grid.ny; ++y)
            if (grid.rowText(y).find(needle) != std::string::npos)
                return y;
    }
    return -1;
}

/// One mouse click: the position frame, the press frame, the release frame, then the
/// frame that answers it. ImGui's event trickle applies one state change per frame, so a
/// down+up pair queued together would never both land.
Grid clickCell(ThreadFixture& fixture, int x, int y, int button) {
    fixture.draw(true, [x, y](ImGuiIO& in) { in.AddMousePosEvent(float(x), float(y)); });
    fixture.draw(true, [button](ImGuiIO& in) { in.AddMouseButtonEvent(button, true); });
    fixture.draw(true, [button](ImGuiIO& in) { in.AddMouseButtonEvent(button, false); });
    return fixture.draw(true);
}

/// Right-click `row` and answer the frame that draws the block menu (the rail's row
/// menu takes the same gesture; button 1 is ImGui's right button).
Grid openBlockMenu(ThreadFixture& fixture, int row) { return clickCell(fixture, 5, row, 1); }

/// The rows in the menu's band that carry `Copy`, near-first: the clicked row, then the
/// row below, then the one above, then a row further out. The popup is anchored at the
/// pointer (clamped to the display edge), so its item is there.
///
/// The band is deliberately tight *and* the callers require exactly one hit: a menu that
/// grew a second item, or a body's own copy control landing in the band (rich_md's code
/// block draws one in the real app), then fails loudly instead of being taken for the
/// menu's item.
std::vector<int> menuCopyRows(const Grid& menu, int clickedRow) {
    static constexpr int kOffsets[] = {0, 1, -1, 2, -2};
    std::vector<int> rows;
    for (const int d : kOffsets) {
        const int y = clickedRow + d;
        if (y >= 0 && y < menu.ny && menu.rowText(y).find("Copy") != std::string::npos)
            rows.push_back(y);
    }
    return rows;
}

/// The ink of `needle`'s first cell on `row`, or -1 when it is not drawn there. A
/// disabled menu item is a different `ImGuiCol`, and on a cell grid a different ink is
/// the only part of "disabled" that is observable - so this is how the test tells a
/// disabled `Copy` from an enabled one that happens to have nothing to copy.
int inkAt(const Grid& grid, int row, const std::string& needle) {
    if (row < 0 || row >= grid.ny)
        return -1;
    const std::size_t at = grid.rowText(row).find(needle);
    if (at == std::string::npos)
        return -1;
    return static_cast<int>(grid.fgAt(static_cast<int>(at), row));
}

/// What one right-click -> `Copy` gesture answered.
struct MenuCopy {
    bool offered = false;        ///< the block menu drew its `Copy` item
    bool uniqueItem = false;     ///< exactly one `Copy` in the band (the menu's own)
    bool askedClipboard = false; ///< the item acted (a disabled one acts on nothing)
    int ink = -1;                ///< the item's ink (see `inkAt()`)
};

MenuCopy copyThroughBlockMenu(ThreadFixture& fixture, int row) {
    MenuCopy out;
    const Grid menu = openBlockMenu(fixture, row);
    const std::vector<int> items = menuCopyRows(menu, row);
    out.offered = !items.empty();
    out.uniqueItem = items.size() == 1;
    if (!out.offered)
        return out;
    const int item = items.front();
    out.ink = inkAt(menu, item, "Copy");
    const std::size_t at = menu.rowText(item).find("Copy");
    g_clipboard.clear();
    g_clipboardSet = false;
    clickCell(fixture, static_cast<int>(at) + 1, item, /*button=*/0);
    out.askedClipboard = g_clipboardSet;
    return out;
}

/// Copy from every block type that has content, through *both* non-Ctrl+C doors: `y` on
/// the focused block (the keyboard path, thread.cpp) and the block menu on an unfocused
/// one (the pointer path). The payloads are the interesting part: a code block copies its
/// code without the fences, a table copies the markdown the model wrote (never the
/// bridge's `llmfun-table` fence), and everything else copies its body verbatim.
void checkCopyFromEveryBlock() {
    installClipboard();

    struct Case {
        const char* what;
        tui::ChatMessageType type;
        const char* summary;
        const char* text;
        const char* thinking;
        bool stream;
        const char* label; ///< the block's header word, the row to act on
        const char* want;  ///< the exact clipboard payload
    };
    const char* table = "| a | b |\n|---|---|\n| 1 | 2 |\n";
    const Case cases[] = {
        // Stored messages, one per v4 type that produces a body row.
        {"user", tui::ChatMessageType::User, "[1] q-one [0.5s]", "user body", "", false, "YOU",
         "user body"},
        {"assistant", tui::ChatMessageType::Assistant, "[2] a-two [1.0s]", "agent prose", "", false,
         "AGENT", "agent prose"},
        {"answer", tui::ChatMessageType::FinalAnswer, "[3] done [1.2s]", "final answer", "", false,
         "ANSWER", "final answer"},
        {"system", tui::ChatMessageType::System, "[4] notice [1.4s]", "a notice", "", false,
         "SYSTEM", "a notice"},
        {"tool call", tui::ChatMessageType::ToolCall, "[5] writeFile: 42 lines [1.6s]",
         "writeFile: 42 lines", "", false, "TOOL", "writeFile: 42 lines"},
        {"tool result", tui::ChatMessageType::ToolResponse, "[6] result: 42 lines [1.8s]",
         "result: 42 lines", "", false, "TOOL", "result: 42 lines"},
        {"code", tui::ChatMessageType::Assistant, "[7] code [2.0s]", "```\nint x = 1;\n```", "",
         false, "AGENT", "int x = 1;\n"},
        {"table", tui::ChatMessageType::Assistant, "[8] table [2.2s]", table, "", false, "AGENT",
         table},
        {"stored thinking", tui::ChatMessageType::Assistant, "[9] think [2.4s]", "the answer",
         "the reasoning", false, "Thinking", "the reasoning"},
        // The live answer: its body and its reasoning are two blocks of one group.
        {"streamed body", tui::ChatMessageType::Assistant, "", "stream-prose", "", true, "AGENT",
         "stream-prose"},
        {"streamed thinking", tui::ChatMessageType::Assistant, "", "stream-prose",
         "stream-reasoning", true, "Thinking", "stream-reasoning"},
    };

    for (const Case& c : cases) {
        const std::string what = c.what;
        const auto build = [&c](ThreadFixture& fixture) {
            if (c.stream)
                fixture.addStream(c.type, c.text, c.thinking);
            else
                fixture.add(message(1, c.type, c.summary, c.text, c.thinking));
        };

        // --- the keyboard door: focus the row, press `y` -------------------------
        ThreadFixture keyboard;
        build(keyboard);
        Grid grid = keyboard.draw();
        const int row = findRow(grid, c.label);
        check(row >= 0, what + ": its row is on screen");
        if (row < 0)
            continue;
        check(grid.rowText(row).find("Copy") == std::string::npos,
              what + ": nothing is focused, so the row carries no Copy chip");
        clickCell(keyboard, 5, row, /*button=*/0);
        grid = keyboard.draw(true);
        check(keyboard.state.ui.thread.focus.valid(), what + ": a click focuses the block");
        check(grid.rowText(row).find("Copy") != std::string::npos,
              what + ": the focused block shows its Copy control");
        g_clipboard.clear();
        g_clipboardSet = false;
        keyboard.draw(true, [](ImGuiIO& in) { in.AddInputCharacter('y'); });
        check(g_clipboardSet && g_clipboard == c.want,
              what + ": `y` copies the block (got '" + g_clipboard + "')");

        // --- the pointer door: the block menu, on an unfocused block -------------
        ThreadFixture pointer;
        build(pointer);
        const Grid before = pointer.draw();
        const int menuRow = findRow(before, c.label);
        check(menuRow >= 0, what + ": its row is on screen for the menu");
        if (menuRow < 0)
            continue;
        const MenuCopy copy = copyThroughBlockMenu(pointer, menuRow);
        check(copy.offered && copy.uniqueItem,
              what + ": the block menu offers exactly one Copy item");
        check(copy.askedClipboard && g_clipboard == c.want,
              what + ": the block menu copies the block (got '" + g_clipboard + "')");
        // The right-click itself changes nothing else: no focus, no toggle.
        check(!pointer.state.ui.thread.focus.valid(),
              what + ": the block menu does not steal the focus");
    }

    // The table payload is the model's markdown, not the bridge's fence.
    check(std::string(table).find("llmfun-table") == std::string::npos,
          "the table case's payload is the model's markdown (the fence is a render detail)");
}

/// A block with no body is focusable - it is a row like any other - but copying it must
/// be a no-op, and the affordances must not promise text it does not have: no chip, and
/// the menu's `Copy` is drawn disabled.
void checkBodylessBlocks() {
    installClipboard();

    // The reference: the same menu item on a block that *has* a body. Its ink is what
    // "enabled" looks like, so the body-less item can be shown to differ (a disabled
    // item is dimmer, and a click that copies nothing would otherwise be indistinguish-
    // able from an enabled item whose body happens to be empty).
    int enabledInk = -1;
    {
        ThreadFixture reference;
        reference.add(message(1, tui::ChatMessageType::User, "[1] x [0.5s]", "x"));
        const Grid before = reference.draw();
        const int row = findRow(before, "YOU");
        check(row >= 0, "the reference block's row is on screen");
        if (row >= 0) {
            const MenuCopy copy = copyThroughBlockMenu(reference, row);
            enabledInk = copy.ink;
            check(copy.offered && copy.uniqueItem && copy.askedClipboard,
                  "the enabled Copy item is the menu's only item and copies (the reference)");
        }
    }

    // The WORK group's own header: `WORK - N steps`, no body of its own.
    {
        ThreadFixture fixture;
        fixture.add(message(1, tui::ChatMessageType::Assistant, "[1] one [1.0s]", "the answer"));
        const model::BlockDesc header = fixture.state.threadAdapter.blockAt(0, 0);
        check(header.kind == model::BlockKind::WorkGroup && !header.hasBody,
              "the work group's header has no body");
        Grid grid = fixture.draw();
        const int row = findRow(grid, "WORK");
        check(row >= 0, "the WORK header is on screen");
        if (row >= 0) {
            clickCell(fixture, 5, row, /*button=*/0);
            grid = fixture.draw(true);
            check(fixture.state.ui.thread.focus == header.id(),
                  "a body-less header is focusable (it is a row of the thread)");
            check(grid.rowText(row).find("Copy") == std::string::npos,
                  "a body-less header shows no Copy control");
            g_clipboard.clear();
            g_clipboardSet = false;
            fixture.draw(true, [](ImGuiIO& in) { in.AddInputCharacter('y'); });
            check(!g_clipboardSet, "`y` on a body-less header copies nothing");
            const MenuCopy copy = copyThroughBlockMenu(fixture, row);
            check(copy.offered && copy.uniqueItem,
                  "the body-less header's menu still offers Copy (as a disabled item)");
            check(!copy.askedClipboard, "the disabled Copy item copies nothing");
            check(enabledInk >= 0 && copy.ink != enabledInk,
                  "the body-less header's Copy item is drawn disabled (a dimmer ink) [" +
                      std::to_string(enabledInk) + " vs " + std::to_string(copy.ink) + "]");
        }
    }

    // The live answer's body row before its first character arrives: present (the row is
    // stable, task 26) and body-less.
    {
        ThreadFixture fixture;
        fixture.addStream(tui::ChatMessageType::Assistant, "", "reasoning only");
        const Grid grid = fixture.draw();
        const int row = findRow(grid, "AGENT");
        check(row >= 0, "a streamed body row exists before its first character");
        if (row >= 0) {
            clickCell(fixture, 5, row, /*button=*/0);
            Grid after = fixture.draw(true);
            check(fixture.state.ui.thread.focus.valid(),
                  "the empty streamed body row is focusable");
            check(after.rowText(row).find("Copy") == std::string::npos,
                  "the empty streamed body row shows no Copy control");
            g_clipboard.clear();
            g_clipboardSet = false;
            fixture.draw(true, [](ImGuiIO& in) { in.AddInputCharacter('y'); });
            check(!g_clipboardSet, "`y` on the empty streamed body copies nothing");
            const MenuCopy copy = copyThroughBlockMenu(fixture, row);
            check(copy.offered && copy.uniqueItem && !copy.askedClipboard,
                  "the empty streamed body's menu item is offered and copies nothing");
        }
    }
}

/// `Ctrl+C` stays exactly what it is today (design §6.7, C-07/D-05): the terminal's
/// `SIGINT`, never a UI chord. Binding it would be dead on the grid (`ISIG` consumes the
/// byte) and would take the pixel profile's own copy chord, so the registry must have no
/// row for it - in any region - while the quit path (`Ctrl+Q`, test_keymap S1-S3) stays.
void checkCopyFeedback() {
    installClipboard();

    ThreadFixture fixture;
    fixture.add(
        message(1, tui::ChatMessageType::User, "[1] first question [0.5s]", "first question"));
    fixture.state.ui.view = model::ThreadView::Ready;
    fixture.draw();
    clickCell(fixture, 5, 0, /*button=*/0);
    fixture.draw(true);
    check(fixture.state.ui.thread.focus.valid(), "the copy fixture has a focused block");
    check(fixture.state.ui.toasts.empty(), "focusing a block says nothing");

    // One press, one item - through the keyboard door.
    g_clipboard.clear();
    fixture.draw(true, [](ImGuiIO& in) { in.AddInputCharacter('y'); });
    check(fixture.state.ui.toasts.size() == 1, "one copy is one feedback item (got " +
                                                   std::to_string(fixture.state.ui.toasts.size()) +
                                                   ")");
    if (!fixture.state.ui.toasts.empty()) {
        const model::Toast& toast = fixture.state.ui.toasts.back();
        check(toast.kind == model::ToastKind::Success, "a copy is a success");
        check(toast.title == "Copied YOU",
              "it names the block that was copied (got '" + toast.title + "')");
        check(toast.detail.empty(), "and carries no second line");
        check(toast.expiresAtMs != 0, "a success expires on its own (the frame's clock drops it)");
    }

    // The same press again refreshes the one item instead of stacking a second.
    fixture.draw(true, [](ImGuiIO& in) { in.AddInputCharacter('y'); });
    check(fixture.state.ui.toasts.size() == 1, "a second copy of the same block is still one item");

    // The chip is the same action: acknowledged first, so the count is this press's own.
    model::dismissShownToast(fixture.state.ui);
    const Grid grid = fixture.draw();
    const std::size_t chipAt = grid.rowText(0).find("Copy");
    check(chipAt != std::string::npos, "the focused block's Copy control is on screen");
    if (chipAt != std::string::npos)
        clickCell(fixture, static_cast<int>(chipAt) + 2, 0, /*button=*/0);
    check(fixture.state.ui.toasts.size() == 1, "the Copy control adds exactly one item (got " +
                                                   std::to_string(fixture.state.ui.toasts.size()) +
                                                   ")");

    // A body-less row: the WORK group's header copies nothing, so it must not claim a copy.
    ThreadFixture header;
    header.add(message(1, tui::ChatMessageType::Assistant, "[1] one [1.0s]", "the answer"));
    Grid headerGrid = header.draw();
    const int row = findRow(headerGrid, "WORK");
    check(row >= 0, "the body-less header is on screen");
    if (row >= 0) {
        clickCell(header, 5, row, /*button=*/0);
        header.draw(true);
        g_clipboard.clear();
        g_clipboardSet = false;
        header.draw(true, [](ImGuiIO& in) { in.AddInputCharacter('y'); });
        check(!g_clipboardSet, "`y` on a body-less header copies nothing");
        check(header.state.ui.toasts.empty(),
              "and raises no feedback item (a copy nobody got is not an answer)");
        // The menu item is offered but disabled (task 44); pressing it is the same no-op.
        const MenuCopy copy = copyThroughBlockMenu(header, row);
        check(!copy.askedClipboard, "the disabled menu item copies nothing");
        check(header.state.ui.toasts.empty(), "and still raises nothing");
    }
}

void checkCtrlCStaysUnbound() {
    const model::Keymap& keymap = model::defaultKeymap();
    bool boundAnywhere = false;
    for (int f = 0; f < static_cast<int>(model::Focus::Count); ++f) {
        if (model::resolve(keymap, ImGuiMod_Ctrl | ImGuiKey_C, static_cast<model::Focus>(f)) !=
            model::Action::None)
            boundAnywhere = true;
    }
    check(!boundAnywhere, "Ctrl+C is not bound in any region (it is the terminal's SIGINT)");
    check(model::resolve(keymap, ImGuiMod_Ctrl | ImGuiKey_Q, model::Focus::None) ==
                  model::Action::Quit &&
              model::resolve(keymap, ImGuiMod_Ctrl | ImGuiKey_Q, model::Focus::Thread) ==
                  model::Action::Quit,
          "the quit chord is still Ctrl+Q, from the shell and from the thread");
}
/// The block menu's *owner* (task 44). The popup id is the block's id scope, and
/// `BlockId`s are recycled by a transcript reset, so an id alone would let a new message
/// inherit an open menu and be copied under it. `UiState::thread.menuBlockId` is what makes
/// the menu's life a block identity: only the block it names submits the popup, and the
/// latch is validated on every drawn frame.
void checkBlockMenuOwnership() {
    installClipboard();

    // Opening latches the owner, and the popup really is an ImGui popup - the fact the
    // frame layer's Esc ownership keys on (`popupOpenLastFrame`, tui.cpp).
    ThreadFixture fixture;
    fixture.add(message(1, tui::ChatMessageType::User, "[1] q-one [0.5s]", "first body"));
    const Grid initial = fixture.draw();
    const int row = findRow(initial, "YOU");
    check(row >= 0, "the row is on screen");
    if (row < 0)
        return;
    const Grid menu = openBlockMenu(fixture, row);
    check(menuCopyRows(menu, row).size() == 1, "the menu drew exactly one Copy item");
    const model::BlockDesc first = fixture.state.threadAdapter.blockAt(0, 0);
    check(fixture.state.ui.thread.menuBlockId == first.id(),
          "opening the menu latches its owner by BlockId");
    check(ImGui::IsPopupOpen(static_cast<ImGuiID>(0),
                             ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel),
          "the block menu is an ImGui popup (the frame layer's Esc latch keys on that)");

    // Esc closes it, and the next drawn frame drops the owner. ImGui closes a popup on Esc
    // only with keyboard nav on (its `NavUpdateCancelRequest`), and the app enables that
    // flag in `tuiRender`; this harness draws the thread directly, so the flag is set for
    // this gesture and restored afterwards (the goldens must not see a nav cursor).
    ImGuiIO& io = ImGui::GetIO();
    const ImGuiConfigFlags savedFlags = io.ConfigFlags;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    fixture.draw(true, [](ImGuiIO& in) { in.AddKeyEvent(ImGuiKey_Escape, true); });
    fixture.draw(true, [](ImGuiIO& in) { in.AddKeyEvent(ImGuiKey_Escape, false); });
    io.ConfigFlags = savedFlags;
    const Grid afterEsc = fixture.draw(true);
    check(menuCopyRows(afterEsc, row).empty(), "Esc closes the block menu");
    check(!fixture.state.ui.thread.menuBlockId.valid(), "a closed menu drops its owner");

    // A clear/session switch recycles the ids (`tuiClearOutput()` calls exactly these two):
    // the owner must be gone and the *new* message must not inherit the old menu.
    ThreadFixture recycled;
    recycled.add(message(1, tui::ChatMessageType::User, "[1] old [0.5s]", "OLD-BODY"));
    const Grid old = recycled.draw();
    const int oldRow = findRow(old, "YOU");
    check(oldRow >= 0, "the old message's row is on screen");
    if (oldRow < 0)
        return;
    const Grid oldMenu = openBlockMenu(recycled, oldRow);
    const std::vector<int> oldItems = menuCopyRows(oldMenu, oldRow);
    check(oldItems.size() == 1, "premise: the old message's menu is open");
    if (oldItems.size() != 1)
        return;
    const int oldItemRow = oldItems.front();
    const std::size_t oldCopyX = oldMenu.rowText(oldItemRow).find("Copy");
    const model::BlockId oldId = recycled.state.ui.thread.menuBlockId;
    recycled.state.threadAdapter.reset();
    model::resetForSessionSwitch(recycled.state.ui);
    recycled.state.chat.outputLines.clear();
    recycled.state.chat.outputLineOpen.clear();
    recycled.state.chat.openWorkGroups.clear();
    recycled.add(message(1, tui::ChatMessageType::User, "[1] new [0.6s]", "NEW-BODY"));
    const Grid fresh = recycled.draw();
    const model::BlockDesc second = recycled.state.threadAdapter.blockAt(0, 0);
    check(second.id() == oldId, "premise: the reset recycled the BlockId");
    check(!recycled.state.ui.thread.menuBlockId.valid(),
          "a transcript reset drops the menu's owner");
    check(menuCopyRows(fresh, oldRow).empty(), "the recycled block does not inherit the menu");
    // And the click the user would have made on the old item cannot copy the new message.
    g_clipboard.clear();
    g_clipboardSet = false;
    clickCell(recycled, static_cast<int>(oldCopyX) + 1, oldItemRow, /*button=*/0);
    recycled.draw(true);
    check(!g_clipboardSet, "a click where the old item was copies nothing (the new body)");
    check(!ImGui::IsPopupOpen(static_cast<ImGuiID>(0),
                              ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel),
          "and the popup is gone");
}

/// The menu does not move the focus, so the keyboard keeps acting on the *focused* block
/// while it is open: `y` copies that block, not the one the menu is anchored to. The rail's
/// row menu behaves the same way (its `m`/`r`/`d`/`p` target the active row, not the row
/// under the pointer), so this pins the intended behaviour rather than a defect.
void checkMenuKeepsTheFocusedBlock() {
    installClipboard();
    ThreadFixture fixture;
    fixture.add(message(1, tui::ChatMessageType::User, "[1] first [0.5s]", "FIRST-BODY"));
    fixture.add(message(2, tui::ChatMessageType::User, "[2] second [0.6s]", "SECOND-BODY"));
    Grid grid = fixture.draw();
    const int first = findRow(grid, "YOU");
    const int second = findRow(grid, "YOU", /*fromBottom=*/true);
    check(first >= 0 && second > first, "both rows are on screen");
    if (first < 0 || second <= first)
        return;
    clickCell(fixture, 5, first, /*button=*/0);
    grid = fixture.draw(true);
    check(fixture.state.ui.thread.focus == model::BlockId{1, 1, model::BlockKind::User},
          "premise: the first block holds the focus");
    const Grid menu = openBlockMenu(fixture, second);
    check(menuCopyRows(menu, second).size() == 1, "the second block's menu is open");
    check(fixture.state.ui.thread.focus == model::BlockId{1, 1, model::BlockKind::User},
          "opening a menu does not move the focus");
    g_clipboard.clear();
    g_clipboardSet = false;
    fixture.draw(true, [](ImGuiIO& in) { in.AddInputCharacter('y'); });
    check(g_clipboardSet && g_clipboard == "FIRST-BODY",
          "`y` while a menu is open copies the focused block (got '" + g_clipboard + "')");
    check(menuCopyRows(fixture.draw(true), second).size() == 1,
          "and the menu stays open on the block it belongs to");
}
// ---------------------------------------------------------------------------
// 9. The goldens
// ---------------------------------------------------------------------------

/// What a screen pins: the glyphs (a text dump), the *inks* (a foreground change is a
/// role or a state, so it must fail a golden exactly like a moved glyph) and the fills.
std::string goldenText(const char* name, const Grid& grid) {
    std::string text = "name: ";
    text += name;
    text += "\nscreen: 100x30\ntext:\n";
    for (int y = 0; y < grid.ny; ++y) {
        std::string row = grid.rowText(y);
        while (!row.empty() && row.back() == ' ')
            row.pop_back();
        text += row;
        text += '\n';
    }
    text += "inks:\n";
    for (int y = 0; y < grid.ny; ++y) {
        std::string runs;
        int start = -1;
        int index = -1;
        for (int x = 0; x <= grid.nx; ++x) {
            const int value =
                x < grid.nx && grid.at(x, y) != 0 ? static_cast<int>(grid.fgAt(x, y)) : -1;
            const bool same = start >= 0 && value == index;
            if (start >= 0 && !same) {
                runs += (runs.empty() ? "" : ", ") + std::to_string(start) + "-" +
                        std::to_string(x - 1) + "=" + std::to_string(index);
                start = -1;
            }
            if (value >= 0 && start < 0) {
                start = x;
                index = value;
            } else if (value < 0) {
                start = -1;
            }
        }
        if (!runs.empty()) {
            text += std::to_string(y);
            text += ": ";
            text += runs;
            text += '\n';
        }
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
        if (!runs.empty()) {
            text += std::to_string(y);
            text += ": ";
            text += runs;
            text += '\n';
        }
    }
    return text;
}

/// One fixture per message type: the screen a golden pins.
struct Scene {
    const char* name;
    std::function<void(ThreadFixture&)> build;
};

std::vector<Scene> scenes() {
    std::vector<Scene> list;
    list.push_back({"user", [](ThreadFixture& f) {
                        f.add(message(1, tui::ChatMessageType::User, "[1] build the project [1.2s]",
                                      "build the project"));
                    }});
    list.push_back({"answer", [](ThreadFixture& f) {
                        f.add(message(1, tui::ChatMessageType::FinalAnswer,
                                      "[1] run make -f tui.mak [3.4s]",
                                      "run make -f tui.mak\nand then the binary"));
                    }});
    list.push_back({"workgroup", [](ThreadFixture& f) {
                        f.add(message(1, tui::ChatMessageType::Assistant,
                                      "[1] looking around [1.0s]", "looking around",
                                      "reasoning about the tree"));
                        f.add(message(2, tui::ChatMessageType::ToolCall,
                                      "[2] tool call: bash ls [1.1s]", "tool call: bash ls"));
                        f.add(message(3, tui::ChatMessageType::ToolResponse,
                                      "[3] tool result \xE2\x9C\x85 writeFile: 42 lines [1.2s]",
                                      "tool result writeFile: 42 lines"));
                    }});
    list.push_back({"toolcall", [](ThreadFixture& f) {
                        f.add(message(1, tui::ChatMessageType::ToolCall,
                                      "[1] tool call: readFile proof.qed [2.0s]",
                                      "tool call: readFile proof.qed"));
                    }});
    list.push_back({"toolresult", [](ThreadFixture& f) {
                        f.add(message(1, tui::ChatMessageType::ToolResponse,
                                      "[1] tool result \xE2\x9D\x8C bash: exit 1 [2.0s]",
                                      "tool result bash: exit 1"));
                    }});
    list.push_back({"thinking", [](ThreadFixture& f) {
                        f.add(message(1, tui::ChatMessageType::Assistant,
                                      "[1] the answer is 42 [3.2s]", "the answer is 42",
                                      "because the machine said so"));
                    }});
    list.push_back({"system", [](ThreadFixture& f) {
                        f.add(message(1, tui::ChatMessageType::System, "[1] session resumed [0.4s]",
                                      "session resumed"));
                    }});
    list.push_back({"code", [](ThreadFixture& f) {
                        f.add(message(1, tui::ChatMessageType::Assistant, "[1] try this [1.0s]",
                                      "```\nint main() { return 0; }\n```"));
                    }});
    return list;
}

void checkGoldens(bool update) {
    // Every golden is captured with one glyph set and one colour depth, whatever the
    // terminal that runs the test answers: the answer is pinned, not the environment.
    for (const Scene& scene : scenes()) {
        ::setenv("LLMFUN_ASCII", "0", 1);
        ui::applyTheme(ui::ThemeId::Graphite, ui::ColorDepth::TrueColor);
        ThreadFixture fixture;
        scene.build(fixture);
        const Grid grid = fixture.draw();
        const std::string text = goldenText(scene.name, grid);
        const std::string path =
            std::string(TEST_BLOCKS_GOLDEN_DIR) + "/blocks-" + scene.name + "-100x30.txt";
        if (update) {
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

    // The two degradations task 26 has to survive: an ASCII terminal (no marks) and a
    // 16-colour one (no exact role inks). Both keep the role as a word and the state as
    // a glyph-plus-word pair, so both screens still draw the same anatomy.
    struct Variant {
        const char* name;
        const char* scene;
        bool ascii;
        ui::ColorDepth depth;
    };
    const Variant variants[] = {
        {"blocks-toolresult-ascii-100x30.txt", "toolresult", true, ui::ColorDepth::TrueColor},
        {"blocks-workgroup-16col-100x30.txt", "workgroup", false, ui::ColorDepth::Ansi16},
    };
    for (const Variant& variant : variants) {
        ::setenv("LLMFUN_ASCII", variant.ascii ? "1" : "0", 1);
        ui::applyTheme(ui::ThemeId::Graphite, variant.depth);
        ThreadFixture fixture;
        for (const Scene& scene : scenes())
            if (std::strcmp(scene.name, variant.scene) == 0)
                scene.build(fixture);
        const Grid grid = fixture.draw();
        const std::string text = goldenText(variant.name, grid);
        const std::string path = std::string(TEST_BLOCKS_GOLDEN_DIR) + "/" + variant.name;
        if (update) {
            std::ofstream out(path);
            out << text;
            out.close();
            check(out.good(), std::string("golden written: ") + path);
            continue;
        }
        std::ifstream in(path);
        check(in.good(), "golden is readable: " + path);
        if (!in.good())
            continue;
        const std::string want((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        check(want == text, std::string("the ") + variant.name + " golden matches");
    }
    // Back to the profile the rest of the run and the next test expect.
    ::setenv("LLMFUN_ASCII", "0", 1);
    ui::applyTheme(ui::ThemeId::Graphite, ui::ColorDepth::TrueColor);
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

    ::setenv("LLMFUN_ASCII", "0", 1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImTui_ImplText_Init();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(kScreenW), static_cast<float>(kScreenH));
    io.DeltaTime = 1.0f / 60.0f;
    ui::applyTheme(ui::ThemeId::Graphite, ui::ColorDepth::TrueColor);

    // One warm-up frame: the atlas is built on the first frame, and a text measure
    // before it answers zero.
    frame([](const ui::Metrics&) {});

    checkSource();
    checkVocabulary();
    checkTiming();
    checkHeaderText();
    checkCopyHelpers();
    checkAnatomy();
    checkThreadDrawsEveryKind();
    checkStreaming();
    checkFocusAndCopy();
    checkStreamReasoningDefault();
    checkCrossGroupIdentity();
    checkCopyFromEveryBlock();
    checkBodylessBlocks();
    checkCopyFeedback();
    checkCtrlCStaysUnbound();
    checkBlockMenuOwnership();
    checkMenuKeepsTheFocusedBlock();
    checkGoldens(update);

    ImTui_ImplText_Shutdown();
    ImGui::DestroyContext();

    std::printf("%s: %d check(s), %d failure(s)\n", g_failures == 0 ? "PASS" : "FAIL", g_checks,
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
