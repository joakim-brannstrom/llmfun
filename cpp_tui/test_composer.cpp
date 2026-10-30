// test_composer.cpp
//
// The composer (views/composer.h, design §4.6 / FR-09, task 27): the paste policy, the
// send rule, the submit handshake, history, auto-grow, the meter, and the view drawn
// through the real text backend.
//
// What this binary proves, in order:
//
//   1. the source. `views/composer.cpp` takes the profile from the metrics (never
//      `tuiIsTextGrid()`), writes no colour literal, reads no display size, names no v4
//      type and does not include `tui.h` - the layering every view is held to
//      (`tools/gate_ratchet.txt`);
//   2. the paste policy (D-PASTE). A CRLF payload folds to LF, a lone CR too, the
//      per-character fold agrees with the whole-payload one, and a payload over 5000
//      characters asks before it is inserted;
//   3. the send rule. The action button always sends; no key does while the key gate is
//      closed, and neither a paste in flight nor an IME composition may send;
//   4. the submit handshake. Trailing NULs and one trailing newline are dropped and a
//      whitespace-only draft is refused - byte for byte what `tui_chat.cpp` does;
//   5. history. `↑`/`↓` only at the draft's first/last line, the draft stashed and handed
//      back, the oldest/newest ends refusing to wrap, and no recall without a caret;
//   6. auto-grow. The row count follows the draft, is clamped to 1..8, and the layout
//      reserves the reported height at every supported size - the composer is never
//      overlapped, and the thread keeps its three rows;
//   7. the meter. The thresholds are 70 %/90 %, an unknown capacity is not 0, and the
//      count the meter prints is the same `formatCount` string the `ctx` chip carries;
//   8. the view. Empty, draft, busy, offline and the paste question drawn into a cell
//      grid: the placeholder and its compact form, `Send`/`Stop`/`Stopping`/`Insert`, the
//      phase and speed while busy, nothing painted outside the composer's rect;
//   9. the interaction. A queued burst is inserted (never submitted), `\r\n` is folded, a
//      trailing newline does not send, a >5000 payload asks and only `Insert` applies it,
//      `Esc` clears then blurs, a busy `Esc` cancels and keeps the draft, `↑` recalls;
//  10. the shell. `renderShell()` derives the composer's inputs from the aggregate,
//      applies a send through the shipped handshake, feeds the grown height back into the
//      layout, and consumes a starter action's hand-off into the draft;
//  11. the goldens. Empty/draft/busy at 100x30 and 40x15, each pinning the glyphs and the
//      painted fills of the composer's cells.
//
// Exit codes: 0 = every check passed; 1 = a check failed; 2 = usage error.
//
//   ./test_composer [-v] [--update] [--print]
//
// `--update` rewrites the fixtures; do that only for a deliberate rendering change and
// commit the diff with it. They are captured with `LLMFUN_ASCII=0`, so a CJK locale or
// `TERM=linux` cannot change a golden. `--print` dumps the grids (a development aid).

#include "views/composer.h"
#include "views/shell.h"

#include "tui.h"

#include "model/status_chips.h"
#include "ui/icons.h"
#include "ui/layout.h"
#include "ui/metrics.h"
#include "ui/text.h"

#include "imtui/imtui.h"

#include "imgui/imgui.h"

#include <algorithm>
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
    std::vector<ImU32> bg;

    std::string rowText(int y) const {
        std::string out;
        for (int x = 0; x < nx; ++x) {
            const uint32_t c = ch[static_cast<std::size_t>(y * nx + x)];
            // A cell the renderer never touched is codepoint 0: it draws as a blank, so
            // the dump and the label search read it as one.
            out += (c == 0)               ? std::string(" ")
                   : (c >= 32 && c < 127) ? std::string(1, static_cast<char>(c))
                                          : utf8Of(c);
        }
        return out;
    }

    bool hasText(const std::string& needle) const {
        for (int y = 0; y < ny; ++y)
            if (rowText(y).find(needle) != std::string::npos)
                return true;
        return false;
    }

    /// The first cell whose glyph starts `needle`, or (-1, -1).
    std::pair<int, int> findText(const std::string& needle) const {
        for (int y = 0; y < ny; ++y) {
            const std::string row = rowText(y);
            const std::size_t at = row.find(needle);
            if (at == std::string::npos)
                continue;
            // The byte offset has to become a *cell* offset: the row is UTF-8.
            int cells = 0;
            for (std::size_t i = 0; i < at; ++i)
                cells += (static_cast<unsigned char>(row[i]) & 0xC0) == 0x80 ? 0 : 1;
            return {cells, y};
        }
        return {-1, -1};
    }

    uint32_t at(int x, int y) const { return ch[static_cast<std::size_t>(y * nx + x)]; }
    ImU32 bgAt(int x, int y) const { return bg[static_cast<std::size_t>(y * nx + x)]; }
};

Grid capture() {
    Grid grid;
    grid.nx = g_screen.nx;
    grid.ny = g_screen.ny;
    grid.ch.resize(static_cast<std::size_t>(grid.nx * grid.ny), 0);
    grid.bg.resize(static_cast<std::size_t>(grid.nx * grid.ny), 0);
    for (int i = 0; i < grid.nx * grid.ny; ++i) {
        grid.ch[static_cast<std::size_t>(i)] = g_screen.data[i].ch;
        grid.bg[static_cast<std::size_t>(i)] = g_screen.data[i].bg;
    }
    return grid;
}

/// One frame of the text grid at `g_w x g_h`: a decoration-free root window, the body
/// drawing inside it, then the frame through the real backend.
///
/// `inject` runs *before* `NewFrame`, because that is when ImGui consumes its input
/// queue - an event added after it would only be seen by the next frame.
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
    ImGui::Begin("composer-frame", nullptr,
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

void clickAt(float x, float y, const std::function<void(const ui::Metrics&)>& body) {
    frame(body, [&](ImGuiIO& io) { io.AddMousePosEvent(x, y); });
    frame(body, [&](ImGuiIO& io) {
        io.AddMousePosEvent(x, y);
        io.AddMouseButtonEvent(0, true);
    });
    frame(body, [&](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); });
    // Park the pointer: a hovered action explains itself with a tooltip, and a tooltip
    // window would paint outside the region this harness checks.
    frame(body, [&](ImGuiIO& io) { io.AddMousePosEvent(0.0f, 0.0f); });
}

/// Park the pointer away from every control.
///
/// A golden (and a "nothing painted outside the rect" check) must not depend on where the
/// previous test's last click left the mouse: a hover changes a button's fill and shows a
/// tooltip, and a tooltip is a window that paints over the grid.
void parkPointer() {
    frame([](const ui::Metrics&) {}, [](ImGuiIO& io) { io.AddMousePosEvent(0.0f, 0.0f); });
}

/// Focus the composer's field the way a user does: a click on the text area (the startup
/// `focusRequested` is a request, not a guarantee in a headless context).
void focusField(const ui::Rect& rect, const std::function<void(const ui::Metrics&)>& body) {
    frame(body);
    clickAt(rect.x + 4.0f, rect.y + 1.0f, body);
}

void pressKey(ImGuiKey key, const std::function<void(const ui::Metrics&)>& body) {
    frame(body, [&](ImGuiIO& io) { io.AddKeyEvent(key, true); });
    frame(body, [&](ImGuiIO& io) { io.AddKeyEvent(key, false); });
}

/// The composer's answers, collected across a multi-frame gesture.
///
/// Every answer is a *one-frame* value: the frame layer consumes it in the frame that
/// produced it, so a harness that clicks (four frames) or presses (two) sees it only if
/// it looks on each frame. `observing()` is that look - it renders the body and then ORs
/// the answers out, which is exactly what `views::applyComposerAnswers()` does.
struct ClickAnswers {
    bool submit = false;
    bool cancel = false;
    bool inserted = false;
    bool discarded = false;
    bool escaped = false;
    bool pasteAsked = false;
    bool palette = false;
};

std::function<void(const ui::Metrics&)>
observing(views::ComposerState& state, ClickAnswers& out,
          const std::function<void(const ui::Metrics&)>& body) {
    return [&state, &out, body](const ui::Metrics& m) {
        body(m);
        out.submit = out.submit || state.submit;
        out.cancel = out.cancel || state.cancelRequested;
        out.inserted = out.inserted || state.pasteInserted;
        out.discarded = out.discarded || state.pasteDiscarded;
        out.escaped = out.escaped || state.escapeConsumed;
        out.pasteAsked = out.pasteAsked || state.paste.ask;
        out.palette = out.palette || state.openPalette;
    };
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

/// The composer's store plus its view state, wired the way `views/shell.cpp` wires them.
struct Fixture {
    std::string buffer;
    std::string draft;
    std::string handoff;
    std::vector<std::string> history;
    int historyPos = -1;
    views::ComposerState state;
    views::ComposerInput input;

    Fixture() {
        input.buffer = &buffer;
        input.history = &history;
        input.historyPos = &historyPos;
        input.draft = &draft;
        input.handoff = &handoff;
        input.state = &state;
        input.contextUsed = 42000;
        input.contextMax = 128000;
        input.model = "qwen3-coder";
        input.nowMs = 1000;
    }

    void resetInput() {
        input.busy = false;
        input.stopping = false;
        input.offline = false;
        input.phase.clear();
        input.speed.clear();
    }
};

/// The rectangle `computeShell()` answers for a composer at `w x h`, with `composerH`
/// reserved - the geometry the frame really gives the view.
ui::Rect composerRect(int w, int h, const ui::Metrics& m, float composerH = 0.0f) {
    ui::LayoutPrefs prefs;
    prefs.railOpen = true;
    prefs.inspectorOpen = true;
    prefs.composerH = composerH;
    const ui::ShellLayout layout =
        ui::computeShell(m, ImVec2(static_cast<float>(w), static_cast<float>(h)), prefs);
    return layout.composer;
}

/// A one-scene body: the composer drawn into the layout's rectangle.
std::function<void(const ui::Metrics&)> bodyFor(Fixture& fixture, ui::Rect& rect) {
    return [&fixture, &rect](const ui::Metrics& m) {
        fixture.input.contextKnown = true;
        views::renderComposer(fixture.input, rect, m);
    };
}

// ---------------------------------------------------------------------------
// 1. The source
// ---------------------------------------------------------------------------

std::string readWhole(const std::string& path) {
    std::ifstream in(path);
    if (!in.good())
        return std::string();
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void checkSource() {
    const std::string source = readWhole(TEST_COMPOSER_SOURCE);
    check(!source.empty(), "views/composer.cpp is readable");
    check(source.find("tuiIsTextGrid") == std::string::npos,
          "the composer takes the profile from the metrics, never from tuiIsTextGrid()");
    check(source.find("ImVec4(") == std::string::npos,
          "the composer writes no colour literal (D-THEME)");
    check(source.find("DisplaySize") == std::string::npos,
          "the composer reads no display size (a view is a geometry taker)");
    check(source.find("ChatMessage") == std::string::npos &&
              source.find("RenderGroup") == std::string::npos,
          "the composer names no v4 type (D-ADAPTER)");
    check(source.find("#include \"tui.h\"") == std::string::npos,
          "the composer does not include tui.h (views/ names no aggregate)");
    check(source.find("IsKeyPressed(ImGuiKey_UpArrow") == std::string::npos,
          "history keys are resolved through the keymap, not read as raw chords");
    // The one raw key read is `Esc`, and it is not a chord: `keymap.h` keeps it out of the
    // table because its step depends on the run and the draft, not on the focus, and the
    // order lives in `model::resolveEscape`. Everything else must go through the registry.
    std::size_t directReads = 0;
    std::size_t escReads = 0;
    const std::string escCall = "IsKeyPressed(ImGuiKey_Escape";
    for (std::size_t at = source.find("IsKeyPressed("); at != std::string::npos;
         at = source.find("IsKeyPressed(", at + 1)) {
        ++directReads;
        if (source.compare(at, escCall.size(), escCall) == 0)
            ++escReads;
    }
    check(directReads == escReads, "the only raw key read in the composer is Esc (" +
                                       std::to_string(directReads) + " reads, " +
                                       std::to_string(escReads) + " of them Esc)");
    // The action's label is the only place the words live: one owner.
    check(source.find("\"Send\"") != std::string::npos &&
              source.find("\"Stopping\"") != std::string::npos,
          "the action labels live in the view (Send/Stopping)");
}

// ---------------------------------------------------------------------------
// 2. The paste policy (D-PASTE)
// ---------------------------------------------------------------------------

void checkPaste() {
    check(views::normalisePaste("a\r\nb") == "a\nb", "CRLF folds to one LF");
    check(views::normalisePaste("a\rb") == "a\nb", "a lone CR folds to LF");
    check(views::normalisePaste("a\nb") == "a\nb", "LF is left alone");
    check(views::normalisePaste("one\r\ntwo\r\nthree") == "one\ntwo\nthree",
          "a three-line CRLF payload keeps its two line breaks");

    // The per-character fold (the widget's char filter) and the whole-payload
    // normaliser must answer the same text: one rule, two call shapes.
    const std::string payloads[] = {"a\r\nb", "a\rb", "a\nb", "\r\n", "\r", "a\r\r\nb", "plain"};
    for (const std::string& payload : payloads) {
        bool afterCr = false;
        std::string folded;
        for (const char c : payload) {
            char32_t out = 0;
            if (views::foldPasteChar(afterCr, static_cast<unsigned char>(c), out))
                folded.push_back(static_cast<char>(out));
        }
        check(folded == views::normalisePaste(payload),
              "the character fold agrees with the payload fold (" + payload + ")");
    }

    {
        const views::PastePlan plan = views::planPaste("one\ntwo\nthree");
        check(!plan.ask, "a three-line paste is inserted without asking");
        check(plan.text == "one\ntwo\nthree", "the three-line paste keeps its text");
        check(views::draftLines(plan.text) == 3, "the three-line paste is three draft lines");
    }
    {
        // A paste that ends in a newline: the text keeps it (it is the user's text) and
        // nothing about it can submit - the send rule only ever answers the action.
        const views::PastePlan plan = views::planPaste("hello\n");
        check(!plan.ask, "a paste ending in a newline does not ask");
        check(plan.text == "hello\n", "the trailing newline is kept in the payload");
    }
    {
        const std::string exactly(views::kPasteAskAboveChars, 'x');
        const std::string oneMore = exactly + "y";
        check(!views::planPaste(exactly).ask, "5000 characters are inserted without asking");
        check(views::planPaste(oneMore).ask, "5001 characters ask first");
        check(views::planPaste(oneMore).text.size() == oneMore.size(),
              "the asked-about payload is kept whole (it can be inserted on `Insert`)");
    }
    {
        // The rule counts *characters*, not bytes: 2000 three-byte CJK codepoints are
        // 6000 bytes and must not ask; 5001 of them must, and the question's number is
        // the codepoint count.
        const std::string cjk2000 = [] {
            std::string text;
            for (int i = 0; i < 2000; ++i)
                text += "\xE4\xB8\xAD"; // U+4E2D
            return text;
        }();
        const std::string cjk5001 = cjk2000 + cjk2000 + [] {
            std::string text;
            for (int i = 0; i < 1001; ++i)
                text += "\xE4\xB8\xAD";
            return text;
        }();
        check(cjk2000.size() == 6000 && ui::utf8Count(cjk2000) == 2000,
              "ui::utf8Count counts codepoints, not bytes (6000 bytes, 2000 characters)");
        const views::PastePlan small = views::planPaste(cjk2000);
        check(!small.ask && small.chars == 2000,
              "2000 CJK characters (6000 bytes) are under the limit (" +
                  std::to_string(small.chars) + ")");
        const views::PastePlan big = views::planPaste(cjk5001);
        check(big.ask && big.chars == 5001,
              "5001 CJK characters ask, and the count is characters (" + std::to_string(big.chars) +
                  ")");
    }
    // The diff a clipboard paste leaves behind: one contiguous insertion, exactly.
    check(views::insertedText("XY", "XabcY") == "abc", "an insertion in the middle is found");
    check(views::insertedText("", "abc") == "abc", "an insertion into an empty draft is found");
    check(views::insertedText("abc", "abc!") == "!", "an appended insertion is found");
    check(views::insertedText("abc", "abc").empty(), "no change is no insertion");
    check(views::insertedText("abc", "ab").empty(), "a deletion is not an insertion");
    check(views::insertedText("abc", "XY") == "XY",
          "a replacement that shares no byte reads as an insertion at the start");
    check(views::insertedText("aXc", "aZZc") == "ZZ",
          "a replacement inside a shared frame is an insertion");
    // The edit the composer replays: `start` and `removed` are bytes, and a CJK pair
    // that shares its first two bytes still cuts on codepoint boundaries.
    {
        const views::TextEdit inserted = views::diffText("XY", "XabcY");
        check(inserted.start == 1 && inserted.removed == 0 && inserted.inserted == "abc",
              "an insertion names its start, its empty removed range and its payload");
        const views::TextEdit replaced = views::diffText("aXc", "aZZc");
        check(replaced.start == 1 && replaced.removed == 1 && replaced.inserted == "ZZ",
              "a replacement names the removed byte too");
        const views::TextEdit cjk = views::diffText("\xE4\xB8\xAD", "\xE4\xB8\xAE");
        check(cjk.start == 0 && cjk.removed == 3 && cjk.inserted == "\xE4\xB8\xAE",
              "a CJK replacement removes the whole old codepoint, not two shared bytes");
    }
    {
        views::PasteState paste;
        paste.inFlightUntilMs = 1500;
        check(views::pasteInFlight(paste, 1000), "a paste is in flight inside its window");
        check(!views::pasteInFlight(paste, 1500), "the window is half-open (1500 is over)");
        check(!views::pasteInFlight(paste, 0), "no clock means no paste window");
        paste.dismiss();
        check(paste.pending.empty() && !paste.ask, "dismiss forgets the question and the payload");
    }
}

// ---------------------------------------------------------------------------
// 3. The send rule (D-KEYGATE's fallback)
// ---------------------------------------------------------------------------

void checkSendRule() {
    const bool k = false;
    using model::SendKey;
    const SendKey settings[] = {SendKey::Enter, SendKey::CtrlEnter, SendKey::EnterNewline};
    for (const SendKey setting : settings) {
        check(views::sendTrigger(/*action=*/true, k, k, k, setting, /*paste=*/false,
                                 /*composing=*/false) == views::SendTrigger::Action,
              "the action button always sends");
        check(views::sendTrigger(/*action=*/false, /*enter=*/true, k, k, setting, false, false) ==
                  views::SendTrigger::None,
              "no key sends while the key gate is closed");
        check(views::sendTrigger(/*action=*/false, k, /*ctrlEnter=*/true, k, setting, false,
                                 false) == views::SendTrigger::None,
              "Ctrl+Enter sends nothing while the key gate is closed");
    }
    check(!views::kEnterSends,
          "the gate constant records plan/key_gate.md's answer (Enter = newline)");
    check(views::sendTrigger(true, true, true, true, SendKey::Enter, /*paste=*/true, false) ==
              views::SendTrigger::Action,
          "a click still sends while a paste is in flight (a click is not a payload)");
    check(views::sendTrigger(false, true, false, false, SendKey::Enter, /*paste=*/true, false) ==
              views::SendTrigger::None,
          "a paste in flight blocks the keyboard send (D-PASTE)");
    check(views::sendTrigger(false, true, false, false, SendKey::Enter, false,
                             /*composing=*/true) == views::SendTrigger::None,
          "an IME composition never sends");
}

// ---------------------------------------------------------------------------
// 4. The submit handshake - byte for byte what tui_chat.cpp does
// ---------------------------------------------------------------------------

void checkSubmit() {
    std::string out;
    check(views::prepareSubmit("hello", out) && out == "hello", "a plain draft is the query");
    check(views::prepareSubmit("hello\n", out) && out == "hello",
          "one trailing newline (Enter in a multiline field) is dropped");
    check(views::prepareSubmit("hello\n\n", out) && out == "hello\n",
          "only one trailing newline is dropped");
    check(!views::prepareSubmit("   \n\t\n", out), "a whitespace-only draft is refused");
    check(!views::prepareSubmit("", out), "an empty draft is refused");
    check(views::prepareSubmit(std::string("abc\0\0", 5), out) && out == "abc",
          "the widget's trailing NULs are trimmed before the newline strip");
    check(views::prepareSubmit(std::string("abc\n\0", 5), out) && out == "abc",
          "a newline *behind* the terminator is still dropped");
    check(views::prepareSubmit("multi\nline", out) && out == "multi\nline",
          "a multi-line draft keeps its inner lines");
}

// ---------------------------------------------------------------------------
// 5. History
// ---------------------------------------------------------------------------

void checkHistory() {
    std::vector<std::string> entries = {"first", "second", "third"};
    int pos = -1;
    std::string draft;
    std::string out;

    check(!views::historyRecall(entries, pos, draft, "draft", -1, -1, 1, out),
          "no caret means no recall (the field is not being edited)");
    check(!views::historyRecall(entries, pos, draft, "draft", -1, 1, 3, out),
          "↑ in the middle of a draft is a caret move, not history");
    check(!views::historyRecall(entries, pos, draft, "draft", 1, 0, 3, out),
          "↓ on the first line of three is a caret move, not history");

    check(views::historyRecall(entries, pos, draft, "draft", -1, 0, 1, out) && out == "third",
          "↑ on the first line recalls the newest query");
    check(draft == "draft", "the draft is stashed on the first recall");
    check(pos == 0, "the history cursor moved one entry");
    check(views::historyRecall(entries, pos, draft, out, -1, 0, 1, out) && out == "second",
          "a second ↑ recalls the one before it");
    check(views::historyRecall(entries, pos, draft, out, -1, 0, 1, out) && out == "first",
          "the oldest query is reachable");
    check(!views::historyRecall(entries, pos, draft, out, -1, 0, 1, out),
          "↑ at the oldest entry does not wrap");
    check(views::historyRecall(entries, pos, draft, out, 1, 0, 1, out) && out == "second",
          "↓ walks back down");
    check(views::historyRecall(entries, pos, draft, out, 1, 0, 1, out) && out == "third",
          "down to the newest entry");
    check(views::historyRecall(entries, pos, draft, out, 1, 0, 1, out) && out == "draft",
          "past the newest entry the draft comes back");
    check(draft.empty() && pos == -1, "the stash is consumed and the cursor is back to live");
    check(!views::historyRecall(entries, pos, draft, "x", 1, 0, 1, out),
          "↓ with no recall in progress is the caret's");

    std::vector<std::string> empty;
    int emptyPos = -1;
    std::string emptyDraft;
    check(!views::historyRecall(empty, emptyPos, emptyDraft, "x", -1, 0, 1, out),
          "an empty history recalls nothing");
}

// ---------------------------------------------------------------------------
// 6. Auto-grow and the layout
// ---------------------------------------------------------------------------

void checkRows() {
    check(views::draftLines("") == 1, "an empty draft is one line");
    check(views::draftLines("a\nb") == 2, "a two-line draft is two lines");
    check(views::draftLines("a\n") == 2, "a trailing newline opens a second line");
    check(views::composerRows("") == 1, "one row when empty");
    check(views::composerRows("a\nb\nc") == 3, "three rows for three lines");
    std::string longDraft;
    for (int i = 0; i < 20; ++i)
        longDraft += "line\n";
    check(views::draftLines(longDraft) == 21, "the raw line count is not clamped");
    check(views::composerRows(longDraft) == views::kComposerMaxRows,
          "the drawn rows are clamped to eight");

    check(views::caretLine("abc", 0) == 0, "a caret at the start is on the first line");
    check(views::caretLine("ab\ncd", 4) == 1, "a caret after the newline is on the second line");
    check(views::caretLine("ab\ncd", 999) == 1, "a caret past the end is clamped");
    check(views::caretLine("", 0) == 0, "an empty draft has one line");

    check(views::lineEmptyAt("", 0), "an empty draft's only line is empty");
    check(!views::lineEmptyAt("abc", 3), "the first line of a one-line draft is not empty");
    check(views::lineEmptyAt("ab\n\ncd", 3), "the empty middle line of a draft is empty");
    check(!views::lineEmptyAt("ab\n\ncd", 2), "the line that holds text is not empty");
    check(!views::lineEmptyAt("ab", 1), "a caret inside text is not on an empty line");

    // The layout: the composer's own reported height is what the thread reserves, so the
    // two rectangles can never overlap - at every supported size, with the composer grown
    // to its maximum.
    const ui::Metrics m = ui::metricsFor(true, 1.0f, 1.0f);
    const int sizes[][2] = {{120, 36}, {100, 30}, {80, 24}, {60, 20}, {40, 15}};
    for (const auto& size : sizes) {
        const int w = size[0];
        const int h = size[1];
        const std::string name = std::to_string(w) + "x" + std::to_string(h);
        for (const float grown : {0.0f, m.composerMinH, m.composerMaxH}) {
            ui::LayoutPrefs prefs;
            prefs.railOpen = true;
            prefs.inspectorOpen = true;
            prefs.composerH = grown;
            const ui::ShellLayout layout =
                ui::computeShell(m, ImVec2(static_cast<float>(w), static_cast<float>(h)), prefs);
            check(layout.composer.h >= m.composerMinH - 0.01f,
                  "the composer keeps its minimum height at " + name);
            check(layout.composer.y >= layout.thread.bottom() - 0.01f,
                  "the thread ends at or above the composer at " + name);
            check(layout.composer.bottom() <= layout.nowBar.y + 0.01f,
                  "the composer ends at or above the now bar at " + name);
            check(layout.thread.h >= 3.0f * m.row - 0.01f,
                  "the thread keeps three rows at " + name);
            check(layout.composer.w > 0.0f && layout.composer.x == 0.0f,
                  "the composer spans the display's width at " + name);
        }
    }

    // The view's own answer: a three-line draft reports six cells (two borders, three text
    // rows, one meta row) and asks the layout for exactly that.
    Fixture fixture;
    fixture.buffer = "one\ntwo\nthree";
    ui::Rect rect = composerRect(100, 30, m);
    const auto body = bodyFor(fixture, rect);
    frame(body);
    check(fixture.state.rows == 1,
          "a composer the layout has not grown yet draws the rows it was given");
    check(std::fabs(fixture.state.height - 6.0f) < 0.01f,
          "the reported height is two borders, three rows and the meta row (" +
              num(fixture.state.height) + ")");
    const ui::Rect grown = composerRect(100, 30, m, fixture.state.height);
    check(std::fabs(grown.h - 6.0f) < 0.01f, "the layout reserves the reported height");
    check(grown.y <= rect.y + 0.01f, "a grown composer starts higher, never lower");
    rect = grown;
    frame(body);
    check(fixture.state.rows == 3, "the frame after the layout grew draws all three text rows");

    fixture.buffer.clear();
    rect = composerRect(100, 30, m, 6.0f);
    frame(body);
    check(fixture.state.rows == 1 && std::fabs(fixture.state.height - 4.0f) < 0.01f,
          "an empty draft asks for the minimum height again");
}

// ---------------------------------------------------------------------------
// 7. The meter
// ---------------------------------------------------------------------------

void checkMeter() {
    check(views::meterLevel(0, 128000) == views::MeterLevel::Normal, "an empty context is normal");
    check(views::meterLevel(69000, 100000) == views::MeterLevel::Normal, "69 % is normal");
    check(views::meterLevel(70000, 100000) == views::MeterLevel::Warn, "70 % is warn");
    check(views::meterLevel(89999, 100000) == views::MeterLevel::Warn, "89.9 % is warn");
    check(views::meterLevel(90000, 100000) == views::MeterLevel::Danger, "90 % is danger");
    check(views::meterLevel(10, 0) == views::MeterLevel::Normal,
          "an unknown capacity is not a full meter");
    check(std::fabs(views::contextFraction(42000, 128000) - 0.328125f) < 1e-5f,
          "the fraction is used/max");
    check(views::contextFraction(200, 100) == 1.0f, "the fraction is clamped to one");
    check(views::contextFraction(-5, 100) == 0.0f, "a negative count is an empty meter");

    // The composer's count and the `ctx` chip must be the same string for the same
    // numbers: both use `ui::formatCount`, which is the whole reason the splitter keeps
    // the numbers (`StatusChips::contextUsed/contextMax`).
    const std::string sentences[] = {"Context: 42000/128000 tokens | Model: qwen3-coder | Ready",
                                     "Context 1/8192 tokens | 38.1 tok/s | Ready",
                                     "Context: 0/0 tokens | Model: none | Ready",
                                     "Context: 999/1000 tokens | Ready"};
    for (const std::string& sentence : sentences) {
        const model::StatusChips chips = model::splitStatusText(sentence);
        const std::size_t slash = chips.context.find('/');
        check(slash != std::string::npos, "the splitter produced a used/max chip");
        check(chips.context.substr(0, slash) == ui::formatCount(chips.contextUsed),
              "the meter's count is the chip's used value (" + chips.context + ")");
        check(chips.context.substr(slash + 1) == ui::formatCount(chips.contextMax),
              "the chip's max is the same number the meter divides by");
    }
}

// ---------------------------------------------------------------------------
// 8. The view, drawn
// ---------------------------------------------------------------------------

/// A scene the goldens pin: a fixture at one size.
struct Scene {
    const char* name;
    int w;
    int h;
    bool busy;
    bool offline;
    bool draft;
};

std::vector<Scene> scenes() {
    return {{"empty", 100, 30, false, false, false}, {"draft", 100, 30, false, false, true},
            {"busy", 100, 30, true, false, true},    {"empty", 40, 15, false, false, false},
            {"draft", 40, 15, false, false, true},   {"busy", 40, 15, true, false, true}};
}

/// Fill a fresh composer scene into `fixture`.
///
/// In place, not by return value: `ComposerInput::state` points into the fixture's own
/// `state` member, so a copied fixture would leave the view writing through a pointer to
/// the *source* object (which is gone) - the kind of aliasing a "return by value" hides
/// until the heap happens to reuse it.
void sceneFixture(const Scene& scene, Fixture& fixture) {
    fixture.buffer.clear();
    fixture.draft.clear();
    fixture.handoff.clear();
    fixture.history.clear();
    fixture.historyPos = -1;
    fixture.state = views::ComposerState{};
    fixture.resetInput();
    fixture.input.contextUsed = 42000;
    fixture.input.contextMax = 128000;
    fixture.input.contextKnown = true;
    fixture.input.model = "qwen3-coder";
    fixture.input.nowMs = 1000;
    if (scene.draft)
        fixture.buffer = "Explain the failing test\nin tui_chat.cpp\nand fix it";
    if (scene.busy) {
        fixture.input.busy = true;
        fixture.input.phase = "Busy";
        fixture.input.speed = "38.1 tok/s";
    }
    fixture.input.offline = scene.offline;
}

void checkView() {
    const ui::Metrics m = ui::metricsFor(true, 1.0f, 1.0f);
    parkPointer(); // no hover in a capture

    // --- empty at 100x30 --------------------------------------------------
    {
        g_w = 100;
        g_h = 30;
        Fixture fixture;
        ui::Rect rect = composerRect(g_w, g_h, m);
        const auto body = bodyFor(fixture, rect);
        const Grid grid = frame(body);
        check(grid.hasText("Ask anything...  / commands"),
              "the empty composer shows the placeholder");
        check(grid.hasText("Send"), "the empty composer shows a real Send action");
        check(!grid.hasText("Stop"), "no Stop while nothing runs");
        check(grid.hasText("qwen3-coder"), "the model is right-aligned on the text line");
        check(grid.hasText("42k"), "the meter shows the used context (42k)");
        check(grid.hasText("ctx"), "the full meter form appears at 100 cells");
        // Nothing outside the region: the view is a geometry taker, and a cell outside its
        // rect would be a composer painted where the layout did not put it.
        bool outside = false;
        for (int y = 0; y < grid.ny; ++y)
            for (int x = 0; x < grid.nx; ++x) {
                const bool inside =
                    x >= static_cast<int>(rect.x) && x < static_cast<int>(rect.right()) &&
                    y >= static_cast<int>(rect.y) && y < static_cast<int>(rect.bottom());
                if (!inside && grid.at(x, y) != 0 && grid.at(x, y) != ' ')
                    outside = true;
            }
        check(!outside, "the composer paints nothing outside its rect");
    }

    // --- empty at 40x15: the compact placeholder, no mode/model -----------
    {
        g_w = 40;
        g_h = 15;
        Fixture fixture;
        ui::Rect rect = composerRect(g_w, g_h, m);
        const auto body = bodyFor(fixture, rect);
        const Grid grid = frame(body);
        check(grid.hasText("Ask..."), "a 40-cell composer shortens the placeholder");
        check(!grid.hasText("Ask anything"), "the long placeholder does not fit at 40 cells");
        check(grid.hasText("Send"), "Send survives the narrow composer");
        check(!grid.hasText("qwen3-coder"), "the model drops before the field does");
        check(grid.hasText("42k"), "the count survives the narrow composer");
        check(!grid.hasText("ctx"), "the bar drops below 80 cells (design §4.6)");
        check(rect.h >= 4.0f, "a 40x15 composer keeps 1 text row + 1 meta row");
    }

    // --- a draft grows the field ------------------------------------------
    {
        g_w = 100;
        g_h = 30;
        Fixture fixture;
        fixture.buffer = "first line\nsecond line\nthird line";
        ui::Rect rect = composerRect(g_w, g_h, m);
        const auto body = bodyFor(fixture, rect);
        frame(body); // the first frame asks the layout to grow
        rect = composerRect(g_w, g_h, m, fixture.state.height);
        const Grid grid = frame(body);
        check(grid.hasText("first line") && grid.hasText("second line") &&
                  grid.hasText("third line"),
              "a three-line draft shows all three lines");
        check(fixture.state.rows == 3, "three lines means three text rows");
    }

    // --- busy: Stop, the phase and the speed ------------------------------
    {
        g_w = 100;
        g_h = 30;
        Fixture fixture;
        fixture.buffer = "one";
        fixture.input.busy = true;
        fixture.input.phase = "Busy";
        fixture.input.speed = "38.1 tok/s";
        ui::Rect rect = composerRect(g_w, g_h, m);
        const auto body = bodyFor(fixture, rect);
        const Grid grid = frame(body);
        check(grid.hasText("Stop"), "a running agent turns the action into Stop");
        check(!grid.hasText("Send"), "Send is gone while the run is active");
        check(grid.hasText("38.1 tok/s"), "the speed is shown in the middle while busy");
        check(grid.hasText("Busy"), "the phase word is shown while busy");
    }

    // --- stopping: the action is disabled ---------------------------------
    {
        g_w = 100;
        g_h = 30;
        Fixture fixture;
        fixture.input.busy = true;
        fixture.input.stopping = true;
        ui::Rect rect = composerRect(g_w, g_h, m);
        const auto body = bodyFor(fixture, rect);
        const Grid grid = frame(body);
        check(grid.hasText("Stopping"), "a requested stop reads Stopping");
    }

    // --- offline: Send stays, disabled, and the field explains itself ------
    {
        g_w = 100;
        g_h = 30;
        Fixture fixture;
        fixture.buffer = "kept";
        fixture.input.offline = true;
        ui::Rect rect = composerRect(g_w, g_h, m);
        const auto body = bodyFor(fixture, rect);
        const Grid grid = frame(body);
        check(grid.hasText("Send"), "an offline composer keeps its action (disabled, not removed)");
        check(grid.hasText("kept"), "the offline draft stays visible");
    }

    // --- the paste question ------------------------------------------------
    {
        g_w = 100;
        g_h = 30;
        Fixture fixture;
        fixture.buffer = "kept draft";
        fixture.state.paste.ask = true;
        fixture.state.paste.pending = std::string(views::kPasteAskAboveChars + 1, 'x');
        fixture.state.paste.question = "Paste 5001 characters?";
        ui::Rect rect = composerRect(g_w, g_h, m);
        const auto body = bodyFor(fixture, rect);
        const Grid grid = frame(body);
        check(grid.hasText("Paste 5001 characters?"), "the paste question replaces the field");
        check(grid.hasText("Insert"), "the action answers the question with Insert");
        check(!grid.hasText("kept draft"), "the draft is not drawn behind the question");
    }

    // --- the placement against the layout, at every size -------------------
    for (const Scene& scene : scenes()) {
        g_w = scene.w;
        g_h = scene.h;
        Fixture fixture;
        sceneFixture(scene, fixture);
        ui::Rect rect = composerRect(g_w, g_h, m, fixture.state.height);
        const auto body = bodyFor(fixture, rect);
        const Grid grid = frame(body);
        check(grid.hasText(scene.busy ? "Stop" : "Send"),
              std::string("the ") + scene.name + " composer shows its action at " +
                  std::to_string(scene.w) + "x" + std::to_string(scene.h));
        // The action sits on the meta row, inside the region's last rows.
        const auto found = grid.findText(scene.busy ? "Stop" : "Send");
        check(found.first >= 0 && found.second >= static_cast<int>(rect.y) &&
                  found.second < static_cast<int>(rect.bottom()) &&
                  found.first < static_cast<int>(rect.right()),
              std::string("the ") + scene.name + " action is inside the composer rect");
    }
}

// ---------------------------------------------------------------------------
// 9. The interaction
// ---------------------------------------------------------------------------

void checkInteraction() {
    const ui::Metrics m = ui::metricsFor(true, 1.0f, 1.0f);
    g_w = 100;
    g_h = 30;

    // --- Send: the answer the frame applies -------------------------------
    {
        Fixture fixture;
        fixture.buffer = "  hello there  ";
        ui::Rect rect = composerRect(g_w, g_h, m);
        const auto body = bodyFor(fixture, rect);
        focusField(rect, body);
        const Grid grid = frame(body);
        check(fixture.state.focused, "a click on the text area focuses the field");
        const auto send = grid.findText("Send");
        check(send.first >= 0, "the Send action is on screen");
        ClickAnswers answers;
        clickAt(static_cast<float>(send.first + 1), static_cast<float>(send.second),
                observing(fixture.state, answers, body));
        check(answers.submit, "a click on Send answers submit");
        check(!answers.pasteAsked, "a send leaves no paste question behind");
    }

    // --- Send with a whitespace-only draft --------------------------------
    {
        Fixture fixture;
        fixture.buffer = "   ";
        ui::Rect rect = composerRect(g_w, g_h, m);
        const auto body = bodyFor(fixture, rect);
        focusField(rect, body);
        const Grid grid = frame(body);
        const auto send = grid.findText("Send");
        ClickAnswers answers;
        clickAt(static_cast<float>(send.first + 1), static_cast<float>(send.second),
                observing(fixture.state, answers, body));
        check(answers.submit,
              "the view still answers submit: the *frame* decides what a blank draft means");
    }

    // --- Stop: the cancel request -----------------------------------------
    {
        Fixture fixture;
        fixture.input.busy = true;
        ui::Rect rect = composerRect(g_w, g_h, m);
        const auto body = bodyFor(fixture, rect);
        frame(body);
        const Grid grid = frame(body);
        const auto stop = grid.findText("Stop");
        check(stop.first >= 0, "the Stop action is on screen while busy");
        ClickAnswers answers;
        clickAt(static_cast<float>(stop.first + 1), static_cast<float>(stop.second),
                observing(fixture.state, answers, body));
        check(answers.cancel, "a click on Stop answers cancelRequested");
        check(!answers.submit, "Stop never sends");
    }

    // --- a burst too small to be a paste is *typing* ----------------------
    {
        // Two characters in one frame (a fast typist on a slow frame, an auto-repeat, an
        // IME commit) must land at the caret, not at the end of the draft: only a burst
        // above `kPasteCaptureAboveChars` takes the capture path.
        Fixture fixture;
        fixture.buffer = "XY";
        ui::Rect rect = composerRect(g_w, g_h, m);
        const auto body = bodyFor(fixture, rect);
        focusField(rect, body);
        pressKey(ImGuiKey_Home, body); // the caret goes to the start of the line
        frame(body, [&](ImGuiIO& io) {
            io.AddInputCharacter('a');
            io.AddInputCharacter('b');
        });
        check(fixture.buffer == "abXY",
              "two characters in one frame are typed at the caret (" + fixture.buffer + ")");
        check(!fixture.state.paste.ask, "two characters are not a paste");

        // A burst split over frames is typing too, in order.
        frame(body, [&](ImGuiIO& io) {
            io.AddInputCharacter('c');
            io.AddInputCharacter('d');
        });
        check(fixture.buffer == "abcdXY",
              "a burst split over frames keeps its order (" + fixture.buffer + ")");
    }

    // --- a real burst pastes at the caret ---------------------------------
    {
        Fixture fixture;
        fixture.buffer = "XY";
        ui::Rect rect = composerRect(g_w, g_h, m);
        const auto body = bodyFor(fixture, rect);
        focusField(rect, body);
        pressKey(ImGuiKey_Home, body);
        const std::string payload(views::kPasteCaptureAboveChars + 2, 'p');
        frame(body, [&](ImGuiIO& io) {
            for (const char c : payload)
                io.AddInputCharacter(static_cast<unsigned short>(c));
        });
        // The run is inserted when the burst ends: one idle frame.
        frame(body);
        check(fixture.buffer == payload + "XY",
              "a captured paste lands at the caret, not at the end (" + fixture.buffer + ")");
    }

    // --- a paste through the character queue ------------------------------
    {
        Fixture fixture;
        ui::Rect rect = composerRect(g_w, g_h, m);
        const auto body = bodyFor(fixture, rect);
        focusField(rect, body);
        // Three characters in one frame is a paste (a person types one per frame).
        frame(body, [&](ImGuiIO& io) {
            for (const char c : std::string("abc"))
                io.AddInputCharacter(static_cast<unsigned short>(c));
        });
        check(fixture.buffer == "abc",
              "a three-character burst is inserted verbatim (" + fixture.buffer + ")");
        check(!fixture.state.submit, "a paste never submits");
        check(!fixture.state.paste.ask, "a short paste does not ask");
    }

    // --- a paste with CRLF ------------------------------------------------
    {
        Fixture fixture;
        ui::Rect rect = composerRect(g_w, g_h, m);
        const auto body = bodyFor(fixture, rect);
        focusField(rect, body);
        frame(body, [&](ImGuiIO& io) {
            for (const char c : std::string("a\r\nb"))
                io.AddInputCharacter(static_cast<unsigned short>(c));
        });
        check(fixture.buffer == "a\nb", "a CRLF paste is folded to LF (" + fixture.buffer + ")");
    }

    // --- a paste ending in a newline --------------------------------------
    {
        Fixture fixture;
        ui::Rect rect = composerRect(g_w, g_h, m);
        const auto body = bodyFor(fixture, rect);
        focusField(rect, body);
        frame(body, [&](ImGuiIO& io) {
            for (const char c : std::string("done\n"))
                io.AddInputCharacter(static_cast<unsigned short>(c));
        });
        check(fixture.buffer == "done\n", "a paste ending in a newline keeps it");
        check(!fixture.state.submit, "a paste ending in a newline does not send");
        // The flight window is opened by *any* multi-character frame, not only by a
        // captured one: a small paste's newline still arrives as an `Enter` a frame or two
        // later (the terminal's trickle), and that key must not be read as a send.
        check(views::pasteInFlight(fixture.state.paste, fixture.input.nowMs),
              "a small paste still opens the flight window");
    }

    // --- a paste over the limit asks; Insert applies it -------------------
    {
        Fixture fixture;
        fixture.buffer = "before:";
        ui::Rect rect = composerRect(g_w, g_h, m);
        const auto body = bodyFor(fixture, rect);
        focusField(rect, body);
        pressKey(ImGuiKey_End, body); // collapse whatever the click selected: append
        const std::string big(views::kPasteAskAboveChars + 1, 'x');
        frame(body, [&](ImGuiIO& io) {
            for (const char c : big)
                io.AddInputCharacter(static_cast<unsigned short>(c));
        });
        frame(body); // the burst ends: the run is planned and asks
        check(fixture.state.paste.ask, "a 5001-character paste asks first");
        check(fixture.buffer == "before:", "nothing reaches the draft before the answer");
        check(fixture.state.paste.pending.size() == big.size(),
              "the pending payload is the whole paste");
        const Grid grid = frame(body);
        const auto insert = grid.findText("Insert");
        check(insert.first >= 0, "the question draws an Insert control");
        // The action keeps its own label while the question is up: a running agent must
        // not lose its Stop control to a paste question (D-CANCEL).
        check(grid.hasText("Send"), "the idle action stays Send while the question is up");
        // Send would send the pre-paste draft and drop the pending payload, so it is
        // disabled while the question is up (Stop, for a busy composer, keeps its state).
        ClickAnswers sendAnswers;
        const auto sendAction = grid.findText("Send");
        clickAt(static_cast<float>(sendAction.first + 1), static_cast<float>(sendAction.second),
                observing(fixture.state, sendAnswers, body));
        check(!sendAnswers.submit, "Send is disabled while the paste question is up");
        check(fixture.state.paste.ask, "...and the question stays up");
        ClickAnswers answers;
        clickAt(static_cast<float>(insert.first + 1), static_cast<float>(insert.second),
                observing(fixture.state, answers, body));
        check(answers.inserted, "Insert answers pasteInserted");
        check(fixture.buffer == "before:" + big,
              "the whole payload lands in the draft (" + std::to_string(fixture.buffer.size()) +
                  ", head '" + fixture.buffer.substr(0, 12) + "')");
        check(!fixture.state.paste.ask, "the question is gone after the insert");
    }

    // --- a paste delivered in several reads is one payload -----------------
    {
        // A terminal hands a big paste over in finite reads (the tty buffer is
        // bounded). The run accumulates the chunks and inserts them once, at the
        // position the first chunk named: the order is the wire's, the caret follows
        // the block, and the limit sees the whole paste.
        Fixture fixture;
        fixture.buffer = "XY";
        ui::Rect rect = composerRect(g_w, g_h, m);
        const auto body = bodyFor(fixture, rect);
        focusField(rect, body);
        pressKey(ImGuiKey_Home, body);
        const std::string first(20, 'a');
        const std::string second(20, 'b');
        frame(body, [&](ImGuiIO& io) {
            for (const char c : first)
                io.AddInputCharacter(static_cast<unsigned short>(c));
        });
        check(fixture.buffer == "XY",
              "a multi-frame paste is not inserted until the burst ends (" + fixture.buffer + ")");
        frame(body, [&](ImGuiIO& io) {
            for (const char c : second)
                io.AddInputCharacter(static_cast<unsigned short>(c));
        });
        check(fixture.buffer == "XY",
              "the run stays out of the draft until the burst ends (" + fixture.buffer + ")");
        // The typed key ends the run: the flush happens before the widget, so the
        // character lands after the whole paste in the same frame.
        frame(body, [](ImGuiIO& io) { io.AddInputCharacter('q'); });
        check(fixture.buffer == first + second + "qXY",
              "the chunks keep their order and the next keystroke follows the run (" +
                  fixture.buffer + ")");
        check(!fixture.state.paste.ask, "40 characters do not ask first");
    }

    // --- a CRLF split across two reads still folds once --------------------
    {
        Fixture fixture;
        ui::Rect rect = composerRect(g_w, g_h, m);
        const auto body = bodyFor(fixture, rect);
        focusField(rect, body);
        const std::string first = std::string(10, 'a') + "\r";
        const std::string second = std::string("\n") + std::string(10, 'b');
        frame(body, [&](ImGuiIO& io) {
            for (const char c : first)
                io.AddInputCharacter(static_cast<unsigned short>(c));
        });
        frame(body, [&](ImGuiIO& io) {
            for (const char c : second)
                io.AddInputCharacter(static_cast<unsigned short>(c));
        });
        frame(body); // the burst ends
        check(fixture.buffer == std::string(10, 'a') + "\n" + std::string(10, 'b'),
              "a CRLF split across reads folds to one newline (" + fixture.buffer + ")");
    }

    // --- a paste delivered in several reads is still limited as one ---------
    {
        Fixture fixture;
        fixture.buffer = "draft:";
        ui::Rect rect = composerRect(g_w, g_h, m);
        const auto body = bodyFor(fixture, rect);
        focusField(rect, body);
        pressKey(ImGuiKey_Home, body);
        const std::string first(3000, 'a');
        const std::string second(3000, 'b');
        frame(body, [&](ImGuiIO& io) {
            for (const char c : first)
                io.AddInputCharacter(static_cast<unsigned short>(c));
        });
        check(!fixture.state.paste.ask, "a 3000-character read is under the limit");
        frame(body, [&](ImGuiIO& io) {
            for (const char c : second)
                io.AddInputCharacter(static_cast<unsigned short>(c));
        });
        frame(body); // the burst ends: 6000 characters, one payload
        check(fixture.state.paste.ask, "a 6000-character paste in two reads asks first");
        check(fixture.buffer == "draft:",
              "the run never reaches the draft before the answer (" + fixture.buffer + ")");
        check(fixture.state.paste.pending == first + second,
              "the pending payload is the whole run (" +
                  std::to_string(fixture.state.paste.pending.size()) + ")");
    }

    // --- a captured paste over a selection replaces it ---------------------
    {
        Fixture fixture;
        fixture.buffer = "hello world";
        ui::Rect rect = composerRect(g_w, g_h, m);
        const auto body = bodyFor(fixture, rect);
        focusField(rect, body);
        pressKey(ImGuiKey_Home, body);
        frame(body, [](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Shift, true); });
        frame(body, [](ImGuiIO& io) {
            io.AddKeyEvent(ImGuiMod_Shift, true);
            io.AddKeyEvent(ImGuiKey_End, true);
        });
        frame(body, [](ImGuiIO& io) {
            io.AddKeyEvent(ImGuiMod_Shift, true);
            io.AddKeyEvent(ImGuiKey_End, false);
        });
        frame(body, [](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Shift, false); });
        const std::string burst(20, 'a');
        frame(body, [&](ImGuiIO& io) {
            for (const char c : burst)
                io.AddInputCharacter(static_cast<unsigned short>(c));
        });
        frame(body); // the burst ends
        check(fixture.buffer == burst,
              "a captured paste replaces the selection (" + fixture.buffer + ")");
    }

    // --- a paste question does not take the Stop control away -------------
    {
        Fixture fixture;
        fixture.input.busy = true;
        ui::Rect rect = composerRect(g_w, g_h, m);
        const auto body = bodyFor(fixture, rect);
        frame(body); // a busy composer is not focused: no field work needed
        const std::string big(views::kPasteAskAboveChars + 1, 'x');
        frame(body, [&](ImGuiIO& io) {
            for (const char c : big)
                io.AddInputCharacter(static_cast<unsigned short>(c));
        });
        const Grid grid = frame(body);
        if (fixture.state.paste.ask) {
            check(grid.hasText("Stop"), "a busy composer keeps Stop while a paste asks");
            const auto stop = grid.findText("Stop");
            ClickAnswers answers;
            clickAt(static_cast<float>(stop.first + 1), static_cast<float>(stop.second),
                    observing(fixture.state, answers, body));
            check(answers.cancel, "Stop still cancels the run while the question is up");
        } else {
            check(false, "the paste question was not raised in a busy composer");
        }
    }

    // --- a paste over the limit, discarded with Esc -----------------------
    {
        Fixture fixture;
        ui::Rect rect = composerRect(g_w, g_h, m);
        const auto body = bodyFor(fixture, rect);
        focusField(rect, body);
        const std::string big(views::kPasteAskAboveChars + 1, 'x');
        frame(body, [&](ImGuiIO& io) {
            for (const char c : big)
                io.AddInputCharacter(static_cast<unsigned short>(c));
        });
        frame(body); // the burst ends: the run is planned and asks
        check(fixture.state.paste.ask, "the question is up");
        ClickAnswers answers;
        pressKey(ImGuiKey_Escape, observing(fixture.state, answers, body));
        check(answers.discarded, "Esc discards the pending paste");
        check(!fixture.state.paste.ask && fixture.buffer.empty(),
              "the discard leaves the draft alone");
        check(answers.escaped, "the press is spent on the question");
    }

    // --- Esc: cancel (busy), clear, blur ----------------------------------
    {
        Fixture fixture;
        fixture.buffer = "half-written query";
        fixture.input.busy = true;
        ui::Rect rect = composerRect(g_w, g_h, m);
        const auto body = bodyFor(fixture, rect);
        focusField(rect, body);
        ClickAnswers cancel;
        pressKey(ImGuiKey_Escape, observing(fixture.state, cancel, body));
        check(cancel.cancel, "Esc with a run active cancels the run");
        check(fixture.buffer == "half-written query", "the cancel keeps the draft");
        check(cancel.escaped, "the cancel press is spent");
        frame(body); // the cancel answer is a one-frame value
        check(!fixture.state.cancelRequested, "the cancel answer is not applied twice");

        fixture.input.busy = false;
        frame(body);
        ClickAnswers clear;
        pressKey(ImGuiKey_Escape, observing(fixture.state, clear, body));
        check(fixture.buffer.empty(), "Esc with an idle run clears the draft");
        check(clear.escaped, "the clear press is spent");
        frame(body);
        ClickAnswers blur;
        pressKey(ImGuiKey_Escape, observing(fixture.state, blur, body));
        check(blur.escaped, "Esc on an empty draft is the blur step");
        check(!fixture.state.focused, "the blur released the field");
    }

    // --- history keys ------------------------------------------------------
    {
        Fixture fixture;
        fixture.history = {"older query", "newer query"};
        fixture.buffer = "my draft";
        ui::Rect rect = composerRect(g_w, g_h, m);
        const auto body = bodyFor(fixture, rect);
        focusField(rect, body);
        pressKey(ImGuiKey_UpArrow, body);
        check(fixture.buffer == "newer query",
              "↑ on the first line recalls the newest query (" + fixture.buffer + ")");
        check(fixture.draft == "my draft", "the draft is stashed");
        check(fixture.historyPos == 0, "the history cursor moved");
        frame(body);
        pressKey(ImGuiKey_UpArrow, body);
        check(fixture.buffer == "older query", "a second ↑ recalls the older query");
        frame(body);
        pressKey(ImGuiKey_DownArrow, body);
        check(fixture.buffer == "newer query", "↓ walks back down");
        frame(body);
        pressKey(ImGuiKey_DownArrow, body);
        check(fixture.buffer == "my draft", "↓ past the newest entry hands the draft back");
        check(fixture.historyPos == -1, "the cursor is live again");

        // `Ctrl+Up` is the shipped alias. The modifier and the arrow are separate frames:
        // ImGui's input trickle applies one transition per key per frame.
        frame(body);
        frame(body, [](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Ctrl, true); });
        frame(body, [](ImGuiIO& io) {
            io.AddKeyEvent(ImGuiMod_Ctrl, true);
            io.AddKeyEvent(ImGuiKey_UpArrow, true);
        });
        check(fixture.buffer == "newer query",
              "Ctrl+Up is the shipped alias (" + fixture.buffer + ")");
        frame(body, [](ImGuiIO& io) {
            io.AddKeyEvent(ImGuiMod_Ctrl, true);
            io.AddKeyEvent(ImGuiKey_UpArrow, false);
        });
        frame(body, [](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Ctrl, false); });

        // The grid's own history path: the two real buttons (the arrows and their
        // modifiers are not delivered by that backend - plan/key_gate.md §4b).
        fixture.historyPos = -1;
        fixture.draft.clear();
        fixture.buffer = "my draft";
        frame(body);
        const Grid grid = frame(body);
        const auto prev = grid.findText("Prev");
        check(prev.first >= 0, "the text grid draws the Prev history button");
        check(grid.hasText("Next"), "the text grid draws the Next history button");
        if (prev.first >= 0) {
            clickAt(static_cast<float>(prev.first + 1), static_cast<float>(prev.second), body);
            check(fixture.buffer == "newer query",
                  "the Prev button recalls the newest query (" + fixture.buffer + ")");
        }
    }

    // --- `/` on an empty line opens the command palette ---------------------
    {
        Fixture fixture;
        ui::Rect rect = composerRect(g_w, g_h, m);
        const auto body = bodyFor(fixture, rect);
        focusField(rect, body);
        ClickAnswers answers;
        frame(observing(fixture.state, answers, body),
              [](ImGuiIO& io) { io.AddInputCharacter('/'); });
        check(answers.palette, "a `/` on an empty line asks for the palette");
        check(fixture.buffer.empty(), "the `/` is taken, not inserted (" + fixture.buffer + ")");

        // Inside a line a slash is an ordinary character. (The click may leave a word
        // selected, so the assertion is "the slash reached the draft", not its exact
        // position.)
        Fixture inLine;
        inLine.buffer = "ls ";
        ui::Rect lineRect = composerRect(g_w, g_h, m);
        const auto lineBody = bodyFor(inLine, lineRect);
        focusField(lineRect, lineBody);
        ClickAnswers lineAnswers;
        frame(observing(inLine.state, lineAnswers, lineBody),
              [](ImGuiIO& io) { io.AddInputCharacter('/'); });
        check(inLine.buffer.find('/') != std::string::npos,
              "a `/` inside a line is typed (" + inLine.buffer + ")");
        check(!lineAnswers.palette, "no palette while the line holds text");

        // A *pasted* slash is data, not the command entry: `/usr/bin/env` in one widget
        // call is a run of characters, so the slash goes back into the draft in front of
        // the rest and no palette opens.
        Fixture pasted;
        ui::Rect pastedRect = composerRect(g_w, g_h, m);
        const auto pastedBody = bodyFor(pasted, pastedRect);
        focusField(pastedRect, pastedBody);
        ClickAnswers pastedAnswers;
        frame(observing(pasted.state, pastedAnswers, pastedBody), [](ImGuiIO& io) {
            for (const char c : std::string("/usr/bin/env"))
                io.AddInputCharacter(static_cast<unsigned short>(c));
        });
        frame(observing(pasted.state, pastedAnswers, pastedBody)); // the burst ends
        check(!pastedAnswers.palette, "a pasted slash does not open the palette");
        check(pasted.buffer == "/usr/bin/env",
              "a pasted slash keeps its place in the payload (" + pasted.buffer + ")");

        // A short pasted slash (at or below the capture threshold) takes the widget
        // path: the filter takes the slash, then puts it back when more characters
        // follow in the same widget call.
        Fixture shortPaste;
        ui::Rect shortRect = composerRect(g_w, g_h, m);
        const auto shortBody = bodyFor(shortPaste, shortRect);
        focusField(shortRect, shortBody);
        ClickAnswers shortAnswers;
        frame(observing(shortPaste.state, shortAnswers, shortBody), [](ImGuiIO& io) {
            for (const char c : std::string("/usr"))
                io.AddInputCharacter(static_cast<unsigned short>(c));
        });
        check(!shortAnswers.palette, "a short pasted slash does not open the palette");
        check(shortPaste.buffer == "/usr",
              "a short pasted slash is put back in front of its payload (" + shortPaste.buffer +
                  ")");
    }

    // --- a hand-off fills the composer without sending ---------------------
    {
        Fixture fixture;
        fixture.handoff = "Plan a feature";
        ui::Rect rect = composerRect(g_w, g_h, m);
        const auto body = bodyFor(fixture, rect);
        frame(body);
        check(fixture.buffer == "Plan a feature", "a hand-off lands in the draft");
        check(fixture.handoff.empty(), "the hand-off is consumed exactly once");
        check(!fixture.state.submit && !fixture.state.paste.ask,
              "a hand-off never sends and never asks");
        check(fixture.historyPos == -1, "a hand-off leaves history navigation");
    }
}

// ---------------------------------------------------------------------------
// 10. The shell wiring
// ---------------------------------------------------------------------------

views::ShellFrame shellFrame(tui::TuiState& state,
                             const std::function<void(ImGuiIO&)>& inject = nullptr) {
    views::ShellFrame out;
    frame(
        [&](const ui::Metrics&) {
            out =
                views::renderShell(state, ImVec2(static_cast<float>(g_w), static_cast<float>(g_h)));
        },
        inject);
    return out;
}

/// The same frame, with the cell grid it painted: the shell's Send action has to be
/// clicked where the *shell* drew it, not where the view's own scene would.
///
/// `answers` collects the composer's one-frame answers (the frame consumes them inside
/// `renderShell`, so a harness has to look on every frame of the gesture).
Grid shellGrid(tui::TuiState& state, views::ShellFrame& out, ClickAnswers* answers = nullptr,
               const std::function<void(ImGuiIO&)>& inject = nullptr) {
    return frame(
        [&](const ui::Metrics&) {
            out =
                views::renderShell(state, ImVec2(static_cast<float>(g_w), static_cast<float>(g_h)));
            if (answers != nullptr) {
                answers->submit = answers->submit || state.composer.submit;
                answers->cancel = answers->cancel || state.composer.cancelRequested;
                answers->escaped = answers->escaped || state.composer.escapeConsumed;
            }
        },
        inject);
}

void checkShell() {
    g_w = 100;
    g_h = 30;
    tui::TuiState state;
    state.sessionPanel.sessions.push_back(tui::SessionEntry{"s1", "Demo session", "", 3, true});
    state.sessionPanel.activeId = "s1";

    // The composer's inputs are derived from the aggregate: the chips' numbers, the run
    // state and the send-key preference.
    state.statusChips = model::splitStatusText("Context: 42000/128000 tokens | 38.1 tok/s | "
                                               "Model: 'qwen3-coder' | Ready");
    state.userQuery.inputBuf = "shell draft";
    views::ShellFrame shell;
    Grid grid = shellGrid(state, shell);
    check(shell.layout.composer.w > 0.0f && shell.layout.composer.h > 0.0f,
          "the shell gives the composer a rectangle");
    check(state.composer.rows == 1, "a one-line draft is one row in the shell's frame");
    check(grid.hasText("shell draft"), "the shell's composer draws the aggregate's draft");
    check(grid.hasText("42k"), "the shell's composer reads the chips' numbers");
    check(grid.hasText("qwen3-coder"), "the shell's composer reads the chips' model");

    // A grown draft is reserved on the *next* frame: the view reports, the layout takes.
    state.userQuery.inputBuf = "one\ntwo\nthree";
    shellGrid(state, shell);
    shell = shellFrame(state);
    check(std::fabs(shell.layout.composer.h - 6.0f) < 0.01f,
          "the shell reserves the composer's grown height (" + num(shell.layout.composer.h) + ")");
    check(shell.layout.composer.y >= shell.layout.thread.bottom() - 0.01f,
          "the thread never draws under the grown composer");

    // Send through the shell: the shipped handshake sets submitReady/submitQuery, pushes
    // history and clears the draft. The click goes where the shell drew the action.
    state.userQuery.inputBuf = "  shipped query \n";
    shellGrid(state, shell);        // the composer reports the height its new draft needs
    grid = shellGrid(state, shell); // the layout has reserved it: the action is where it will be
    const auto send = grid.findText("Send");
    check(send.first >= 0, "the shell's composer draws a Send action");
    ClickAnswers answers;
    if (send.first >= 0) {
        const float x = static_cast<float>(send.first + 1);
        const float y = static_cast<float>(send.second);
        shellGrid(state, shell, &answers, [&](ImGuiIO& io) { io.AddMousePosEvent(x, y); });
        shellGrid(state, shell, &answers, [&](ImGuiIO& io) {
            io.AddMousePosEvent(x, y);
            io.AddMouseButtonEvent(0, true);
        });
        shellGrid(state, shell, &answers, [&](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); });
    }
    // The frame consumes the answer in the same frame it is produced (`applyComposerAnswers`
    // clears it), so the observable result is the handshake it started - not a flag.
    check(state.userQuery.submitReady, "a click on the shell's Send starts the submit handshake");
    check(state.userQuery.submitQuery == "  shipped query ",
          "the handshake keeps the draft's own bytes and drops only the newline (" +
              state.userQuery.submitQuery + ")");
    check(state.userQuery.inputBuf.empty(), "the draft is cleared after a send");
    check(!state.userQuery.inputHistory.empty() &&
              state.userQuery.inputHistory.back() == "  shipped query ",
          "the sent query is recallable");
    check(state.userQuery.historyPos == -1, "a send leaves history navigation");
    // The answer is one frame's: the next frame must not send again.
    state.userQuery.submitReady = false;
    shellGrid(state, shell);
    check(!state.userQuery.submitReady, "the send answer is not applied twice");

    // A starter action's hand-off: the thread writes the one-shot field, the composer's
    // next frame applies it and never sends.
    state.userQuery.newInputBufString = "Plan a feature";
    shellGrid(state, shell);
    check(state.userQuery.inputBuf == "Plan a feature",
          "a starter action's prompt lands in the shell's draft");
    check(state.userQuery.newInputBufString.empty(), "the hand-off is consumed");
    check(!state.userQuery.submitReady, "a hand-off never sends");

    // The busy composer: Stop, and the Esc cancel goes through `tuiRequestCancel`.
    tui::tuiSetReadyStatus(state, false);
    grid = shellGrid(state, shell);
    check(grid.hasText("Stop"), "a running agent shows Stop in the shell's composer");
    tui::tuiSetReadyStatus(state, true);

    // `/` on an empty line opens the palette's command entry through the shell.
    state.userQuery.inputBuf.clear();
    shellGrid(state, shell);
    grid = shellGrid(state, shell);
    check(!state.palette.open, "the palette starts closed");
    shellGrid(state, shell, nullptr, [](ImGuiIO& io) { io.AddInputCharacter('/'); });
    check(state.palette.open, "a `/` on the composer's empty line opens the palette");
    check(state.userQuery.inputBuf.empty(), "the `/` never reaches the draft");
    views::closePalette(state.palette, state);
    shellGrid(state, shell);
    check(!state.palette.open, "the palette closes again");
    check(state.composer.focusRequested || state.composer.focused,
          "closing the palette hands the keyboard back to the composer");

    // One `Esc` in the shell is one step end to end: the composer spends the press, and
    // `applyComposerAnswers()` marks `state.escapeConsumed` so the frame-level chain
    // stands down (D-ESC). Without that the frame would cancel *and* the composer would
    // clear on the same press.
    state.escapeConsumed = false;
    state.userQuery.inputBuf = "a draft to clear";
    shellGrid(state, shell); // the field takes the keyboard
    shellGrid(state, shell);
    shellGrid(state, shell, nullptr, [](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Escape, true); });
    check(state.escapeConsumed, "the composer's Esc marks the press spent for the frame");
    check(state.userQuery.inputBuf.empty(), "and the clear step ran on that press");
    shellGrid(state, shell, nullptr, [](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Escape, false); });
    state.escapeConsumed = false;
}

// ---------------------------------------------------------------------------
// 10b. The pixel profile's clipboard paste (ImGui's own insertion)
// ---------------------------------------------------------------------------

std::string g_clipboard;

const char* clipboardGet(void*) { return g_clipboard.c_str(); }
void clipboardSet(void*, const char* text) { g_clipboard = text != nullptr ? text : ""; }

/// Ctrl+V is ImGui's clipboard insertion (`stb_textedit_paste`), not a burst of queued
/// characters: the composer sees the characters through its filter but never gets to
/// hold them back. The post-widget size guard is what applies D-PASTE's limit there, so
/// the test drives the real shortcut with a local clipboard.
void checkClipboardPaste() {
    g_w = 100;
    g_h = 30;
    const ui::Metrics m = ui::metricsFor(true, 1.0f, 1.0f);
    ImGuiIO& io = ImGui::GetIO();
    io.GetClipboardTextFn = clipboardGet;
    io.SetClipboardTextFn = clipboardSet;
    io.ClipboardUserData = nullptr;

    const auto pressPaste = [](const std::function<void(const ui::Metrics&)>& body) {
        frame(body, [](ImGuiIO& io2) { io2.AddKeyEvent(ImGuiMod_Ctrl, true); });
        frame(body, [](ImGuiIO& io2) {
            io2.AddKeyEvent(ImGuiMod_Ctrl, true);
            io2.AddKeyEvent(ImGuiKey_V, true);
        });
        frame(body, [](ImGuiIO& io2) {
            io2.AddKeyEvent(ImGuiMod_Ctrl, true);
            io2.AddKeyEvent(ImGuiKey_V, false);
        });
        frame(body, [](ImGuiIO& io2) { io2.AddKeyEvent(ImGuiMod_Ctrl, false); });
    };

    const auto selectAll = [](const std::function<void(const ui::Metrics&)>& body) {
        frame(body, [](ImGuiIO& io2) { io2.AddKeyEvent(ImGuiMod_Shift, true); });
        frame(body, [](ImGuiIO& io2) {
            io2.AddKeyEvent(ImGuiMod_Shift, true);
            io2.AddKeyEvent(ImGuiKey_End, true);
        });
        frame(body, [](ImGuiIO& io2) {
            io2.AddKeyEvent(ImGuiMod_Shift, true);
            io2.AddKeyEvent(ImGuiKey_End, false);
        });
        frame(body, [](ImGuiIO& io2) { io2.AddKeyEvent(ImGuiMod_Shift, false); });
    };

    const auto clickInsert = [](Fixture& fixture,
                                const std::function<void(const ui::Metrics&)>& body) {
        const Grid grid = frame(body);
        const auto insert = grid.findText("Insert");
        if (insert.first < 0)
            return false;
        ClickAnswers answers;
        clickAt(static_cast<float>(insert.first + 1), static_cast<float>(insert.second),
                observing(fixture.state, answers, body));
        return answers.inserted;
    };

    // A small clipboard paste is inserted (and CRLF folded) - the ordinary case.
    {
        Fixture fixture;
        fixture.buffer = "XY";
        ui::Rect rect = composerRect(g_w, g_h, m);
        const auto body = bodyFor(fixture, rect);
        focusField(rect, body);
        pressKey(ImGuiKey_Home, body);
        g_clipboard = "a\r\nb";
        pressPaste(body);
        check(fixture.buffer == "a\nbXY",
              "a small clipboard paste lands at the caret, CRLF folded (" + fixture.buffer + ")");
    }

    // A clipboard paste over the limit is undone and asked about.
    {
        Fixture fixture;
        fixture.buffer = "draft:";
        ui::Rect rect = composerRect(g_w, g_h, m);
        const auto body = bodyFor(fixture, rect);
        focusField(rect, body);
        g_clipboard = std::string(views::kPasteAskAboveChars + 1, 'z');
        pressPaste(body);
        // The shortcut has its own timeline (modifier, key, releases): give the paste the
        // frames it needs, or the question would land in the *next* section's fixture.
        for (int i = 0; i < 4 && !fixture.state.paste.ask; ++i)
            frame(body);
        check(fixture.state.paste.ask, "a clipboard paste over the limit asks (" +
                                           std::to_string(fixture.state.paste.pending.size()) +
                                           ")");
        check(fixture.buffer == "draft:",
              "...and the draft is left as it stood (" + fixture.buffer + ")");
        check(fixture.state.paste.pending.size() == g_clipboard.size(),
              "the pending payload is the whole clipboard");
    }

    // A clipboard paste over a selection replays the replacement: the reverted draft
    // still holds the selected text, so `Insert` must erase it, not insert in front of it.
    {
        Fixture fixture;
        fixture.buffer = "hello world";
        ui::Rect rect = composerRect(g_w, g_h, m);
        const auto body = bodyFor(fixture, rect);
        focusField(rect, body);
        pressKey(ImGuiKey_Home, body);
        selectAll(body);
        g_clipboard = std::string(views::kPasteAskAboveChars + 1, 'z');
        pressPaste(body);
        for (int i = 0; i < 4 && !fixture.state.paste.ask; ++i)
            frame(body);
        check(fixture.state.paste.ask, "a clipboard paste over a selection asks");
        check(fixture.buffer == "hello world", "...and the selection is restored first");
        check(clickInsert(fixture, body), "Insert answers the selection paste");
        check(fixture.buffer == g_clipboard,
              "Insert replaces the selection instead of duplicating it (" +
                  std::to_string(fixture.buffer.size()) + ")");
    }

    // The question's Insert puts the caret after the payload, not at the end of the
    // draft: the next keystroke must land where the paste ended.
    {
        Fixture fixture;
        fixture.buffer = "AB";
        ui::Rect rect = composerRect(g_w, g_h, m);
        const auto body = bodyFor(fixture, rect);
        focusField(rect, body);
        pressKey(ImGuiKey_Home, body);
        pressKey(ImGuiKey_RightArrow, body); // caret 1, between A and B
        g_clipboard = std::string(views::kPasteAskAboveChars + 1, 'z');
        pressPaste(body);
        for (int i = 0; i < 4 && !fixture.state.paste.ask; ++i)
            frame(body);
        check(fixture.state.paste.ask, "the caret paste asks");
        check(clickInsert(fixture, body), "Insert answers the caret paste");
        frame(body, [](ImGuiIO& io2) { io2.AddInputCharacter('q'); });
        check(fixture.buffer == "A" + g_clipboard + "qB",
              "the next keystroke lands after the inserted payload (" +
                  fixture.buffer.substr(0, 3) + "..." +
                  fixture.buffer.substr(fixture.buffer.size() - 3) + ")");
    }

    // A replacement that shares its first two bytes with the old codepoint: 中
    // (E4 B8 AD) -> 丮 (E4 B8 AE). A byte diff would start the payload on a
    // continuation byte; the codepoint diff replaces the whole codepoint.
    {
        Fixture fixture;
        fixture.buffer = "\xE4\xB8\xAD";
        ui::Rect rect = composerRect(g_w, g_h, m);
        const auto body = bodyFor(fixture, rect);
        focusField(rect, body);
        pressKey(ImGuiKey_Home, body);
        selectAll(body);
        g_clipboard = std::string("\xE4\xB8\xAE") + std::string(5001, 'z');
        pressPaste(body);
        for (int i = 0; i < 4 && !fixture.state.paste.ask; ++i)
            frame(body);
        check(fixture.state.paste.ask, "a CJK replacement paste asks");
        check(clickInsert(fixture, body), "Insert answers the CJK replacement");
        check(fixture.buffer == g_clipboard,
              "the CJK replacement is whole and on a codepoint boundary (" +
                  std::to_string(fixture.buffer.size()) + " bytes)");
        check(ui::utf8Count(fixture.buffer) == 5002,
              "the replaced draft is 5002 characters, not a partial sequence");
    }
    // Empty the clipboard and flush the shortcut *before* the next section: a paste is a
    // key event plus a clipboard read, and a frame after the gesture is where a repeat
    // would land. The flush body draws nothing, so there is no field to paste into.
    g_clipboard.clear();
    for (int i = 0; i < 3; ++i)
        frame([](const ui::Metrics&) {});
    io.GetClipboardTextFn = nullptr;
    io.SetClipboardTextFn = nullptr;
}

// ---------------------------------------------------------------------------
// 11. The goldens
// ---------------------------------------------------------------------------

std::string goldenText(const Scene& scene) {
    const ui::Metrics m = ui::metricsFor(true, 1.0f, 1.0f);
    g_w = scene.w;
    g_h = scene.h;
    Fixture fixture;
    sceneFixture(scene, fixture);
    parkPointer(); // the pointer belongs to the previous test, not to this golden
    // Two passes: the first frame reports the height the draft needs, the second draws in
    // the rectangle the layout reserves for it - a golden of a three-line draft that only
    // had one row to draw in would pin the wrong screen.
    ui::Rect rect = composerRect(g_w, g_h, m);
    const auto body = bodyFor(fixture, rect);
    frame(body);
    rect = composerRect(g_w, g_h, m, fixture.state.height);
    const Grid grid = frame(body);
    std::string text = "name: ";
    text += scene.name;
    text += "\nscreen: " + std::to_string(scene.w) + "x" + std::to_string(scene.h);
    text +=
        "\ncomposer: " + num(rect.x) + "," + num(rect.y) + " " + num(rect.w) + "x" + num(rect.h);
    text +=
        "\nrows: " + std::to_string(fixture.state.rows) + " height: " + num(fixture.state.height);
    text += "\ntext:\n";
    for (int y = 0; y < grid.ny; ++y) {
        std::string row = grid.rowText(y);
        while (!row.empty() && row.back() == ' ')
            row.pop_back();
        text += row;
        text += '\n';
    }
    // A fill is half of what a container is: the painted background is recorded as runs
    // of equal colour index, one line per row that has any.
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
    const std::string dir = TEST_COMPOSER_GOLDEN_DIR;
    for (const Scene& scene : scenes()) {
        const std::string path = dir + "/composer-" + scene.name + "-" + std::to_string(scene.w) +
                                 "x" + std::to_string(scene.h) + ".txt";
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
            check(true, std::string("the ") + scene.name + " " + std::to_string(scene.w) + "x" +
                            std::to_string(scene.h) + " golden matches");
            continue;
        }
        std::size_t line = 1;
        std::size_t at = 0;
        while (at < expected.size() && at < text.size() && expected[at] == text[at]) {
            line += expected[at] == '\n' ? 1 : 0;
            ++at;
        }
        check(false, std::string("the ") + scene.name + " " + std::to_string(scene.w) + "x" +
                         std::to_string(scene.h) + " golden matches (first difference on line " +
                         std::to_string(line) + ": " + path + ")");
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

    // One glyph set for every frame, whatever terminal runs this: `LLMFUN_ASCII=0`
    // outranks TERM and the locale in the detection order, so a golden cannot depend on
    // the environment.
    ::setenv("LLMFUN_ASCII", "0", 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImTui_ImplText_Init();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(g_w), static_cast<float>(g_h));
    io.DeltaTime = 1.0f / 60.0f;

    // One warm-up frame: the atlas is built on the first frame, and a text measure before
    // it answers zero.
    frame([](const ui::Metrics&) {});

    checkSource();
    checkPaste();
    checkSendRule();
    checkSubmit();
    checkHistory();
    checkRows();
    checkMeter();
    checkView();
    checkInteraction();
    checkShell();
    checkClipboardPaste();
    checkGoldens(update);

    ImTui_ImplText_Shutdown();
    ImGui::DestroyContext();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("%d checks passed\n", g_checks);
    return 0;
}
