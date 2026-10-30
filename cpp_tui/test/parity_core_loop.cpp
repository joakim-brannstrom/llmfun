// parity_core_loop.cpp
//
// The merge gate's core loop (task 40, design §8 / D-PARITY): one scripted session through the
// workbench's real frame pipeline, asserted step by step, plus the golden screens at the four
// reference grid sizes and the performance gate.
//
//     start -> session select -> ask -> stream -> stop mid-stream -> answer -> copy ->
//     agents tab -> activity tab -> raw log open/close -> settings change -> quit
//
// Everything runs against the shipped code: `tuiRender()` (the frame the D loop drives), the C
// ABI D links against (`tuiAddChatMessage`, `tuiUpdateStreamChatMessage`,
// `tuiPipelineAgentUpdate`, `tuiAddLogRecord`, `tuiTakeCancel`, `tuiIsSubmitReady`, ...), the
// keymap dispatcher, the composer's and the palette's own key handling. No step asserts on a
// fixture the harness built behind the UI's back: a click, a chord or a typed line is what moves
// the state, and every assertion reads either the resulting `TuiState` or the cell grid the frame
// painted.
//
// Three things this binary is, in order:
//
//   1. THE LOOP. Twelve steps, one per line of the design's core-loop clause. Each step is
//      asserted where it lands: the queued session action, the submit handshake, the live
//      answer's clock, the stop request (and its consume-once), the answer in the store, the
//      clipboard payload, the inspector's tab, the Activity rows, the Raw Log's open flag, the
//      persisted preference and the quit handshake.
//
//   2. THE GOLDENS. A fixed scene - three sessions with no timestamps, a two-message thread, an
//      idle status, an empty composer - drawn by the whole shell at 120x36, 100x30, 80x24 and
//      60x20 and compared against `test/golden/parity-shell-<WxH>.txt` (glyphs *and* the painted
//      fills, the composer-golden format). `--update` rewrites them; that belongs in the same
//      commit as the deliberate rendering change. The GUI's screen is captured as a P6 PPM under
//      `--out` instead of a text golden, because a pixel frame is not comparable across drivers
//      (Q-05's documented fallback).
//
//   3. THE PERF GATE. The fixture set the design names - 1000 messages, 2000 sessions, 5000 raw
//      records (and the 500 Activity rows the projection keeps out of them) - rendered headless on
//      one build. The workbench must stay inside the absolute budget `plan/baseline.md` recorded
//      and inside 10 % of the last measured *pre-removal* workbench run (task 40 recorded it beside
//      the other renderer's numbers; task 41 removed that renderer, so only this one is left).
//      Session open (state +
//      fixture + first frame) is measured the same way.
//
// The two backends. The grid is the default: the real imtui text backend with the text-mode
// markdown profile armed (`initMarkdownTextMode()` before `ImTui_ImplText_Init()`, the 1-cell
// bake), so a golden is what a terminal shows. `--gui` drives the real pixel backend under xvfb
// (`makeGuiBackend` + `initMarkdownGuiMode`, 1280x820), which is where the backend-independent
// steps run against a framebuffer and the window-level facts live. Two steps need a *pointer* in
// the grid (the rail row and the composer's `Send`) and have no pixel coordinate this harness
// could know; the GUI run takes their keyboard-only equivalents - the palette's session row and
// its free-text row - and both runs assert the same outcome, because both write the same answer
// (`UiState::rail.selectId`, the composer's submit handshake). The composer's own controls are
// pinned in both profiles by `./test_composer`.
//
// Exit codes: 0 = every check passed; 1 = a check failed (the failing step and the grid row are
// named on stderr); 2 = usage or environment error.
//
//   ./parity_core_loop [-v] [--print] [--update]
//   xvfb-run -s "-screen 0 1280x820x24" ./parity_core_loop --gui --out DIR
//
// `--update` rewrites the four text goldens and does nothing else: the loop and the perf gate
// still run, so a change that breaks a step cannot be "fixed" by re-baselining a screen.
//
// Environment: `LLMFUN_ASCII` is pinned to 0, `COLORTERM` to truecolor (the depth the goldens'
// fills are recorded at) and `TZ` to UTC, so neither the glyph set, the colour depth nor the
// civil day can change a golden. The settings step's `ui.conf` lands in `./parity_scratch`,
// removed at start.

#include "tui.h"
#include "tui_api.h"
#include "tui_backend.h"

#include "model/settings.h"
#include "model/stream_state.h"
#include "model/thread_view.h"
#include "model/ui_state.h"

#include "views/composer.h"
#include "views/inspector.h"
#include "views/palette.h"
#include "views/rawlog.h"
#include "views/settings.h"
#include "views/shell.h"

#include "ui/layout.h"
#include "ui/metrics.h"
#include "ui/tokens.h"

#include "imtui/imtui.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

// `TuiState`: the ABI handle's private shape, shared with the other contract tests (one copy of
// the layout for all of them). The C functions below take the handle, the frame takes the
// aggregate - the same pairing `./test_nowbar` uses.
#include "test/tui_state_handle.h"

namespace ui = llmfun::tui::ui;
namespace views = llmfun::tui::views;
namespace model = llmfun::tui::model;
namespace tui = llmfun::tui;

namespace {

// ---------------------------------------------------------------------------
// Checks
// ---------------------------------------------------------------------------

int g_checks = 0;
int g_failures = 0;
int g_frames = 0;
bool g_verbose = false;
bool g_print = false;
const char* g_phase = "startup";

void check(bool condition, const std::string& what) {
    ++g_checks;
    if (condition) {
        if (g_verbose)
            std::printf("ok   %s\n", what.c_str());
        return;
    }
    std::fprintf(stderr, "FAIL [%s]: %s\n", g_phase, what.c_str());
    ++g_failures;
}

void phase(const char* name) {
    g_phase = name;
    if (g_verbose)
        std::printf("--- %s\n", name);
}

[[noreturn]] void usage(const char* argv0, const std::string& problem) {
    std::fprintf(stderr,
                 "usage: %s [-v] [--print] [--update]\n"
                 "       %s --gui --out DIR [-v]\n"
                 "  (%s)\n",
                 argv0, argv0, problem.c_str());
    std::exit(2);
}

std::string num(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.3f", value);
    std::string text = buffer;
    while (!text.empty() && text.back() == '0')
        text.pop_back();
    if (!text.empty() && text.back() == '.')
        text.pop_back();
    return text;
}

std::string quoted(const std::string& text) { return "'" + text + "'"; }

// ---------------------------------------------------------------------------
// The cell grid the text backend writes (the `TScreen` pattern of
// ./test_session_filter_smoke, ./test_nowbar and the composer harness)
// ---------------------------------------------------------------------------

ImTui::TScreen g_screen;

int g_w = 120;
int g_h = 36;

std::string utf8Of(unsigned cp) {
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

/// One captured frame: the glyph of every cell and the background index behind it.
struct Grid {
    int nx = 0;
    int ny = 0;
    std::vector<unsigned> ch;
    std::vector<unsigned char> bg;

    unsigned at(int x, int y) const {
        if (x < 0 || y < 0 || x >= nx || y >= ny)
            return 0;
        return ch[static_cast<std::size_t>(y) * nx + x];
    }
    unsigned char bgAt(int x, int y) const {
        if (x < 0 || y < 0 || x >= nx || y >= ny)
            return 0;
        return bg[static_cast<std::size_t>(y) * nx + x];
    }
    std::string rowText(int y) const {
        std::string text;
        for (int x = 0; x < nx; ++x) {
            const unsigned cp = at(x, y);
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

    /// The **cell** (not the byte) of the first occurrence of `needle`, top to bottom then left to
    /// right - the coordinate a click takes.
    ///
    /// `rowText()` is UTF-8, so a byte offset is not a column: `▸ Beta` starts at byte 6 and cell
    /// 4, and a click one column off lands on a neighbour (which then reads as a UI bug). The
    /// conversion walks the row's cells and adds each glyph's encoded length until it reaches the
    /// match.
    bool findText(const std::string& needle, int& outX, int& outY) const {
        for (int y = 0; y < ny; ++y) {
            const std::string row = rowText(y);
            const std::size_t byteAt = row.find(needle);
            if (byteAt == std::string::npos)
                continue;
            std::size_t bytes = 0;
            int cell = -1;
            for (int x = 0; x < nx; ++x) {
                if (bytes == byteAt) {
                    cell = x;
                    break;
                }
                if (bytes > byteAt)
                    break;
                const unsigned cp = at(x, y);
                bytes += utf8Of(cp == 0 ? ' ' : cp).size();
            }
            outX = cell >= 0 ? cell : 0;
            outY = y;
            return true;
        }
        return false;
    }

    /// Every row's text, for a failing check's dump.
    std::string dump() const {
        std::string text;
        for (int y = 0; y < ny; ++y) {
            std::string row = rowText(y);
            while (!row.empty() && row.back() == ' ')
                row.pop_back();
            text += std::to_string(y) + ": " + row + "\n";
        }
        return text;
    }
};

Grid capture() {
    Grid grid;
    grid.nx = g_screen.nx;
    grid.ny = g_screen.ny;
    const std::size_t cells = static_cast<std::size_t>(grid.nx) * grid.ny;
    grid.ch.assign(cells, 0);
    grid.bg.assign(cells, 0);
    for (std::size_t i = 0; i < cells; ++i) {
        grid.ch[i] = g_screen.data[i].ch;
        grid.bg[i] = g_screen.data[i].bg;
    }
    return grid;
}

/// The clipboard the UI last wrote.
///
/// Two readers, one meaning, because the two profiles really do have two clipboards:
///
///  * the text grid has none at all - the harness never starts the ncurses backend, so the only
///    platform function is the sink installed on `PlatformIO` (which is what
///    `ImGui::SetClipboardText()` calls), and the check is "the UI asked for exactly this string";
///  * the pixel profile has the real one: the GLFW backend installs
///  `Platform_Set/GetClipboardTextFn`
///    (`glfwSet/GetClipboardString`), so the run reads its own write back through the platform -
///    the desktop hop the manual script finishes (`tools/parity_manual.md` §1) is the only part
///    left.
std::string g_clipboard;

void clipboardSink(ImGuiContext* /*ctx*/, const char* text) {
    g_clipboard = text != nullptr ? text : "";
}

// ---------------------------------------------------------------------------
// The harness: one state, one frame driver, two backends
// ---------------------------------------------------------------------------

enum class Mode { Text, Gui };

/// The scripted session's state plus everything a step needs to drive it.
///
/// The state is heap-owned so `shutdown` can release it (and the backend it owns) *before* the
/// ImGui context goes away - a `TuiState` member would outlive `DestroyContext()`.
struct Harness {
    Mode mode = Mode::Text;
    std::unique_ptr<tui::TuiState> core{new tui::TuiState()};
    std::unique_ptr<::TuiState> handle{new ::TuiState{core.get()}};
    std::string iniPath;
    Grid last;
    /// The frame that asked to quit: `tuiRender()` answered "user terminated", which is what the
    /// loop's last step asserts.
    bool quit = false;

    bool text() const { return mode == Mode::Text; }

    /// One frame: the input events, `ImGui::NewFrame()` (through the backend), the real
    /// `tuiRender()`, then the present step (the grid's draw-data replay or the GUI swap).
    void frame(const std::function<void(ImGuiIO&)>& inject = nullptr) {
        ImGuiIO& io = ImGui::GetIO();
        if (text()) {
            io.DisplaySize = ImVec2(static_cast<float>(g_w), static_cast<float>(g_h));
            io.DeltaTime = 1.0f / 60.0f;
        }
        if (inject)
            inject(io);
        if (text()) {
            ImTui_ImplText_NewFrame();
            ImGui::NewFrame();
            const bool keepGoing = tui::tuiRender(*core);
            ImGui::Render();
            ImTui_ImplText_RenderDrawData(ImGui::GetDrawData(), &g_screen);
            last = capture();
            ++g_frames;
            if (g_print) {
                std::printf("--- frame %dx%d [%s]\n", g_w, g_h, g_phase);
                std::printf("%s", last.dump().c_str());
            }
            if (!keepGoing)
                quit = true;
            return;
        }
        core->backend->newFrame();
        const bool keepGoing = tui::tuiRender(*core);
        core->backend->renderFrame();
        ++g_frames;
        if (!keepGoing)
            quit = true;
    }

    void idle(int count = 1) {
        for (int i = 0; i < count; ++i)
            frame();
    }

    /// A physical key press: down on one frame, up on the next (the pair the ncurses backend
    /// produces per reported event, and the pair GLFW produces for a tap).
    void pressKey(ImGuiKey key) {
        frame([&](ImGuiIO& io) { io.AddKeyEvent(key, true); });
        frame([&](ImGuiIO& io) { io.AddKeyEvent(key, false); });
    }

    /// `Ctrl+<key>`: the modifier goes down one frame before the key and up one frame after, so
    /// `IsKeyChordPressed()` sees the chord rather than a bare key that happened to arrive while
    /// the modifier was still queued.
    void pressCtrl(ImGuiKey key) {
        frame([](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Ctrl, true); });
        frame([&](ImGuiIO& io) { io.AddKeyEvent(key, true); });
        frame([&](ImGuiIO& io) { io.AddKeyEvent(key, false); });
        frame([](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Ctrl, false); });
    }

    /// `Ctrl+Shift+<key>`: the pixel profile's chord form (a terminal drops the Shift, which is why
    /// the grid's paths are the typed command and the palette row - `plan/key_gate.md` §10).
    void pressCtrlShift(ImGuiKey key) {
        frame([](ImGuiIO& io) {
            io.AddKeyEvent(ImGuiMod_Ctrl, true);
            io.AddKeyEvent(ImGuiMod_Shift, true);
        });
        frame([&](ImGuiIO& io) { io.AddKeyEvent(key, true); });
        frame([&](ImGuiIO& io) { io.AddKeyEvent(key, false); });
        frame([](ImGuiIO& io) {
            io.AddKeyEvent(ImGuiMod_Shift, false);
            io.AddKeyEvent(ImGuiMod_Ctrl, false);
        });
    }

    /// Type a string, one *character* per frame: the composer reads a large burst in one frame as
    /// a paste (D-PASTE), and a step that typed a query as one burst would be exercising that path
    /// instead of the field.
    ///
    /// The bytes are decoded first: a multi-byte sequence sent one byte at a time is a run of
    /// invalid code points, and the composer would then rightly drop them - a failure that would
    /// read as a composer bug. The decode mirrors the ncurses backend (wchar -> UTF-8 -> one
    /// `AddInputCharacter` per codepoint).
    void typeText(const std::string& text) {
        for (std::size_t i = 0; i < text.size();) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            unsigned cp = 0;
            int len = 0;
            if (c < 0x80) {
                cp = c;
                len = 1;
            } else if ((c & 0xE0) == 0xC0) {
                cp = c & 0x1F;
                len = 2;
            } else if ((c & 0xF0) == 0xE0) {
                cp = c & 0x0F;
                len = 3;
            } else if ((c & 0xF8) == 0xF0) {
                cp = c & 0x07;
                len = 4;
            }
            if (len == 0 || i + static_cast<std::size_t>(len) > text.size()) {
                ++i; // invalid or truncated: skip (the fixtures are valid UTF-8)
                continue;
            }
            for (int k = 1; k < len; ++k)
                cp = (cp << 6) |
                     (static_cast<unsigned char>(text[i + static_cast<std::size_t>(k)]) & 0x3F);
            const unsigned codepoint = cp;
            frame([&](ImGuiIO& io) { io.AddInputCharacter(codepoint); });
            i += static_cast<std::size_t>(len);
        }
    }

    /// A click: press on one frame, release on the next at the same cell (ImGui's buttons act on
    /// the release edge). The grid's coordinates are cells, which is what every layout number in
    /// this profile is; a GUI run has pixels and no cell arithmetic, so its steps are the
    /// keyboard ones.
    void click(int x, int y) {
        if (!text())
            return;
        frame([&](ImGuiIO& io) {
            io.MousePos = ImVec2(static_cast<float>(x), static_cast<float>(y));
            io.MouseDown[0] = true;
        });
        frame([&](ImGuiIO& io) {
            io.MousePos = ImVec2(static_cast<float>(x), static_cast<float>(y));
            io.MouseDown[0] = false;
        });
    }

    /// Park the pointer away from every control: a hover left over from an earlier step would
    /// draw a row's hover affordance into a golden that is not about hovering.
    void parkPointer() {
        if (!text())
            return;
        frame([&](ImGuiIO& io) {
            io.MousePos = ImVec2(-1.0f, -1.0f);
            io.MouseDown[0] = false;
            io.MouseDown[1] = false;
        });
    }

    bool findOnScreen(const std::string& needle, int& x, int& y) const {
        return last.findText(needle, x, y);
    }

    /// Click the text `needle` (the first occurrence); false when it is not on screen.
    bool clickOn(const std::string& needle) {
        int x = 0, y = 0;
        if (!findOnScreen(needle, x, y))
            return false;
        click(x + 1, y);
        return true;
    }
};

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

/// What the UI last put on the clipboard (the two readers are documented at `g_clipboard`).
/// Defined here because it needs the whole `Harness`.
std::string readClipboard(Harness& h) {
    if (h.text())
        return g_clipboard;
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    if (pio.Platform_GetClipboardTextFn == nullptr)
        return std::string();
    const char* text = pio.Platform_GetClipboardTextFn(ImGui::GetCurrentContext());
    return text != nullptr ? std::string(text) : std::string();
}

std::string readFileText(const std::string& path, bool& ok) {
    std::ifstream in(path);
    if (!in.good()) {
        ok = false;
        return std::string();
    }
    ok = true;
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool writeFileText(const std::string& path, const std::string& text) {
    std::ofstream out(path);
    if (!out.good())
        return false;
    out << text;
    return out.good();
}

bool fileContains(const std::string& path, const std::string& needle) {
    bool ok = false;
    const std::string text = readFileText(path, ok);
    return ok && text.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// Fixtures - always through the C ABI, i.e. the way D writes them
// ---------------------------------------------------------------------------

String makeStr(const char* text) {
    String s;
    s.data = text;
    s.len = text != nullptr ? std::strlen(text) : 0;
    return s;
}

String makeStr(const std::string& text) { return makeStr(text.c_str()); }

/// One snapshot row.
///
/// The strings are `const char*` on purpose: a `const std::string&` parameter would be bound to a
/// temporary at every call site (`makeSession("id", "title", ...)`), and the `String` the caller
/// keeps would point at the destroyed temporary - a snapshot of dangling pointers that the rail
/// then draws as garbage. A literal, or a `c_str()` of storage the caller owns, outlives the call
/// that copies it.
SessionItem makeSession(const char* id, const char* title, const char* preview, std::size_t count,
                        int active, long long updatedAtMs) {
    SessionItem item{};
    item.id = makeStr(id);
    item.title = makeStr(title);
    item.preview = makeStr(preview);
    item.messageCount = count;
    item.isActive = active;
    item.updatedAtMs = updatedAtMs;
    item.flags = 0;
    item.reserved = 0;
    return item;
}

/// `tuiSetSessionList` copies every string during the call, but the `SessionItem`s themselves
/// have to outlive it - so the caller owns the vector.
void setSessions(Harness& h, const std::vector<SessionItem>& items) {
    tuiSetSessionList(h.handle.get(), items.data(), items.size());
}

void addMessage(Harness& h, TuiChatMessageType type, const std::string& summary,
                const std::string& text, const std::string& thinking = std::string()) {
    ChatMessageParam param{};
    param.summary = makeStr(summary);
    param.text = makeStr(text);
    param.thinking = makeStr(thinking);
    param.type = type;
    tuiAddChatMessage(h.handle.get(), param);
}

void updateStream(Harness& h, const std::string& text) {
    ChatMessageParam param{};
    param.summary = makeStr("");
    param.text = makeStr(text);
    param.thinking = makeStr("");
    param.type = TuiChatMessageType_Assistant;
    tuiUpdateStreamChatMessage(h.handle.get(), param);
}

void addLogRecord(Harness& h, TuiLogLevel level, const std::string& message,
                  const std::string& detail = std::string(), const std::string& module = "app") {
    TuiLogRecord record{};
    record.level = level;
    record.action = TuiLogAction_None;
    record.timestampMs = 0; // the UI stamps the arrival
    record.line = 0;
    record.reserved = 0;
    record.message = makeStr(message);
    record.detail = makeStr(detail);
    record.module = makeStr(module);
    record.function = makeStr("");
    record.file = makeStr("");
    record.actionPayload = makeStr("");
    tuiAddLogRecord(h.handle.get(), &record);
}

void updateAgent(Harness& h, const std::string& id, const std::string& role,
                 const std::string& content, const std::string& tool, long long contextUsed) {
    PipelineChatMessage msg{};
    msg.content = makeStr(content);
    msg.reasoning = makeStr("");
    msg.role = makeStr(role);
    msg.finishReason = makeStr("");
    msg.status = makeStr("");
    msg.currentTool = makeStr(tool);
    msg.contextUsed = contextUsed;
    msg.contextMax = 8192;
    msg.reserved = 0;
    tuiPipelineAgentUpdate(h.handle.get(), makeStr(id), msg);
}

/// Select a palette row by its stable id (the `kId*` literals of `views/palette.cpp`, and the
/// session ids themselves for the session rows).
///
/// The row is only *selected* here and activated by the `Enter` a step presses, so the activation
/// path is the shipped one (`renderPalette()` -> `palettePerformRow()`); a query would be the
/// other way to aim, but a query ranks a different row the moment a label changes, and a parity
/// step must not depend on copy.
bool selectPaletteRow(Harness& h, const std::string& id) {
    views::PaletteState& p = h.core->palette;
    for (std::size_t i = 0; i < p.visible.size(); ++i) {
        const int index = p.visible[i];
        if (index < 0 || static_cast<std::size_t>(index) >= p.items.size())
            continue;
        if (id == p.items[static_cast<std::size_t>(index)].id) {
            p.selected = static_cast<int>(i);
            return true;
        }
    }
    return false;
}

/// Open the palette and activate the row `id`: the two keys a user presses (`Ctrl+K`, `Enter`)
/// with the target named by identity.
bool runPaletteRow(Harness& h, const std::string& id) {
    h.pressCtrl(ImGuiKey_K);
    if (!h.core->palette.open)
        return false;
    h.idle(1);
    if (!selectPaletteRow(h, id))
        return false;
    h.pressKey(ImGuiKey_Enter);
    h.idle(1);
    return true;
}

// ---------------------------------------------------------------------------
// Harness lifecycle
// ---------------------------------------------------------------------------

/// A fresh scratch directory for the settings step's `ui.conf`.
///
/// A previous run's file would be *read* by the first workbench frame, and the settings step's
/// assertions would then depend on the last run's choices.
void resetScratchDir(const std::string& dir) {
    std::remove((dir + "/ui.conf").c_str());
    std::remove((dir + "/parity.ini").c_str());
    ::mkdir(dir.c_str(), 0755);
}

void harnessInitText(Harness& h) {
    h.mode = Mode::Text;
    std::setlocale(LC_ALL, "");
    // One glyph set and one colour depth for every run: `LLMFUN_ASCII=0` outranks TERM and the
    // locale in the detection order, and `COLORTERM=truecolor` pins the 24-bit palette the
    // goldens' fills are recorded in.
    ::setenv("LLMFUN_ASCII", "0", 1);
    ::setenv("COLORTERM", "truecolor", 1);
    ::setenv("TZ", "UTC", 1);
    ::tzset();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    // The text-mode markdown profile must be armed *before* the backend init: the backend builds
    // and locks the font atlas, and a later bake would resolve to the wrong size.
    tui::initMarkdownTextMode(*h.core);
    ImTui_ImplText_Init();
    tui::applyUiTheme();
    tui::finishMarkdownTextMode(*h.core);
    ImGui::GetIO().DisplaySize = ImVec2(static_cast<float>(g_w), static_cast<float>(g_h));

    ImGui::GetPlatformIO().Platform_SetClipboardTextFn = &clipboardSink;
}

void harnessShutdownText(Harness& h) {
    h.handle.reset();
    // The rich_md context is the state's, and the wrapper's process-wide slot must let go of it
    // before the state does - otherwise the next harness inherits a dangling pointer.
    if (h.core)
        tui::destroyMarkdown(*h.core);
    h.core.reset();
    ImTui_ImplText_Shutdown();
    ImGui::DestroyContext();
}

void harnessInitGui(Harness& h, const std::string& iniPath) {
    h.mode = Mode::Gui;
    h.iniPath = iniPath;
    std::setlocale(LC_ALL, "");
    ::setenv("TZ", "UTC", 1);
    ::tzset();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;

    // 1280x820 is D-GEOM's window (D-GOLDEN's pixel size). It comes from the settings file the
    // backend loads, written next to the capture so nothing outside the output directory is
    // touched.
    tui::TuiSettings view;
    view.fontScaleMain = 1.0f;
    view.windowWidth = 1280;
    view.windowHeight = 820;
    tui::saveTuiSettings(view, iniPath);

    std::string error;
    std::unique_ptr<tui::Backend> backend = tui::makeGuiBackend(iniPath, h.core->settings);
    if (!backend->init(error)) {
        std::fprintf(stderr, "parity_core_loop: the GUI backend did not start (no display?): %s\n",
                     error.c_str());
        ImGui::DestroyContext();
        std::exit(2);
    }
    h.core->backend = std::move(backend);
    tui::initMarkdownGuiMode(*h.core);
    tui::applyUiTheme();
    // The clipboard is deliberately *not* intercepted here: the pixel profile's own
    // `Platform_Set/GetClipboardTextFn` (GLFW) is what the copy step reads back, so the check
    // covers the real platform hop and not just the string the UI asked for.
}

void harnessShutdownGui(Harness& h) {
    h.handle.reset();
    if (h.core && h.core->backend) {
        h.core->backend->shutdown(); // the window, the GL context and the ImGui backends
        h.core->backend.reset();
    }
    h.core.reset();
    ImGui::DestroyContext();
}

// ---------------------------------------------------------------------------
// The core loop
// ---------------------------------------------------------------------------

/// Step 1 - start: the workbench shell is up, with an empty thread and a real composer.
void stepStart(Harness& h) {
    phase("1. start");
    h.idle(2);

    check(h.core->ui.view == model::ThreadView::Empty,
          "start: an empty session shows the empty state");
    // The shell's own anatomy: the top bar, the empty-state card, the composer's control and the
    // now bar. Only the shell draws these words, so this is also the "the workbench is what is
    // on screen" check.
    if (h.text()) {
        check(h.last.hasText("llmfun"), "start: the top bar's wordmark is drawn");
        check(h.last.hasText(model::kStarterActions[0].label),
              "start: the empty state offers its starter actions");
        check(h.last.hasText(model::kPaletteHint), "start: the empty state keeps the palette hint");
        check(h.last.hasText("Ask anything"), "start: the composer shows its placeholder");
        check(h.last.hasText("Send"), "start: the composer's action is Send while idle");
    }
    check(h.core->ui.prefs.rawLog == model::kRawLogOff,
          "start: the Raw Log level is the shipped default (off)");
    check(!h.core->ui.rawLogOpen, "start: the Raw Log is closed on every start (D-RAWLOG)");
    check(h.core->ui.prefs.tab == model::InspectorTab::Activity,
          "start: the inspector's default tab is Activity");
}

/// Step 2 - session select: a row press queues the one action D understands, and the replay lands
/// in the thread.
void stepSelectSession(Harness& h) {
    phase("2. session select");
    std::vector<SessionItem> sessions;
    sessions.push_back(
        makeSession("20260815-100000-aaaa", "Alpha session", "first user preview", 3, 1, 0));
    sessions.push_back(
        makeSession("20260815-100000-bbbb", "Beta session", "second user preview", 5, 0, 0));
    sessions.push_back(makeSession("20260815-100000-cccc", "Gamma session", "", 0, 0, 0));
    setSessions(h, sessions);
    h.idle(2);

    check(h.core->sessionPanel.sessions.size() == 3, "select: the snapshot reached the rail");
    if (h.text()) {
        check(h.last.hasText("Beta session"), "select: the rail draws the snapshot's titles");
    }

    // The grid's path is the pointer, as a user has it; the pixel profile's is the palette's
    // session row - the same answer (`UiState::rail.selectId`), a keyboard-only route.
    if (h.text()) {
        check(h.clickOn("Beta session"), "select: the Beta row is on screen to click");
    } else {
        check(runPaletteRow(h, "20260815-100000-bbbb"),
              "select: the palette's session row was activated");
    }
    h.idle(2);

    const bool queued = tuiIsSessionActionReady(h.handle.get()) == 1;
    check(queued, "select: a row press queues exactly one session action");
    if (queued) {
        SessionAction action = tuiGetSessionAction(h.handle.get());
        check(action.type == TuiSessionAction_Select, "select: the action is Select");
        const std::string id = action.sessionId.data != nullptr
                                   ? std::string(action.sessionId.data, action.sessionId.len)
                                   : std::string();
        check(id == "20260815-100000-bbbb",
              "select: the action names the row that was clicked (" + id + ")");
        String_Free(action.sessionId);
        String_Free(action.title);
    }
    check(tuiIsSessionActionReady(h.handle.get()) == 0, "select: that read drained the queue");

    // D's half of a switch: the snapshot with the new active id, the thread cleared, the replayed
    // messages. Both are the shipped calls, not a fixture written behind the UI.
    std::vector<SessionItem> switched;
    switched.push_back(
        makeSession("20260815-100000-aaaa", "Alpha session", "first user preview", 3, 0, 0));
    switched.push_back(
        makeSession("20260815-100000-bbbb", "Beta session", "second user preview", 5, 1, 0));
    switched.push_back(makeSession("20260815-100000-cccc", "Gamma session", "", 0, 0, 0));
    setSessions(h, switched);
    tuiClearChatMessages(h.handle.get());
    addMessage(h, TuiChatMessageType_User, "[1] build the project", "build the project");
    addMessage(h, TuiChatMessageType_Assistant, "[2] running the build", "running the build");
    h.idle(2);

    check(h.core->sessionPanel.activeId == "20260815-100000-bbbb",
          "select: the switched-to session is the active row");
    check(h.core->ui.view == model::ThreadView::Ready,
          "select: the replayed transcript is the thread's body");
    if (h.text()) {
        check(h.last.hasText("build the project"), "select: the replayed user message is drawn");
        check(h.last.hasText("running the build"), "select: the replayed work row is drawn");
    }
}

/// Step 3 - ask: the composer's own path (click, type, Send) hands D one query.
void stepAsk(Harness& h) {
    phase("3. ask");
    const std::string query = "summarise the plan";

    if (h.text()) {
        check(h.clickOn("Ask anything"), "ask: the composer's field is on screen to click");
        h.idle(1);
        h.typeText(query);
        check(h.core->userQuery.inputBuf == query,
              "ask: the typed query is the composer's draft (" +
                  quoted(h.core->userQuery.inputBuf) + ")");
        check(h.clickOn("Send"), "ask: the Send action is on screen to click");
        h.idle(1);
    } else {
        // The pixel profile's route to the same handshake: the palette's free-text row submits
        // the line (no pointer coordinate for the Send button exists in this harness). The row is
        // selected by identity, so a ranking change cannot make the step submit something else.
        h.pressCtrl(ImGuiKey_K);
        h.idle(1);
        check(h.core->palette.open, "ask: Ctrl+K opens the palette");
        h.typeText(query);
        check(selectPaletteRow(h, "ask.freeText"), "ask: the free-text row exists for the query");
        h.pressKey(ImGuiKey_Enter);
        h.idle(2);
    }

    check(tuiIsSubmitReady(h.handle.get()) == 1, "ask: the submit handshake fired");
    check(h.core->userQuery.submitQuery == query, "ask: D receives the query the user typed (" +
                                                      quoted(h.core->userQuery.submitQuery) + ")");
    check(h.core->userQuery.inputBuf.empty(), "ask: the draft is spent");
    check(!h.core->userQuery.inputHistory.empty() && h.core->userQuery.inputHistory.back() == query,
          "ask: the query is recallable from the composer's history");

    tuiResetSubmit(h.handle.get());
    check(tuiIsSubmitReady(h.handle.get()) == 0, "ask: D's acknowledgement clears the flag");
}

/// Step 4 - stream: a run's live answer arrives through the ABI, the clock runs, and the
/// composer's action becomes Stop.
void stepStream(Harness& h) {
    phase("4. stream");
    tuiReadyStatus(h.handle.get(), 0);
    h.idle(1);

    updateStream(h, "# Plan\n\n1. read the tree");
    h.idle(1);
    updateStream(h, "# Plan\n\n1. read the tree\n2. make it");
    h.idle(3); // the store's 33 ms window: one frame parks a snapshot, the next applies it

    check(h.core->chat.hasStreamMsg, "stream: the live answer is in the store");
    check(h.core->ui.thread.stream.active(), "stream: the live answer's clock started");
    check(!h.core->readyStatus, "stream: the frame knows a run is in flight");
    if (h.text()) {
        check(h.last.hasText("read the tree"), "stream: the streamed text is drawn");
        check(h.last.hasText("Stop"),
              "stream: the composer's action is Stop while a run is active");
    }
}

/// Step 5 - stop mid-stream: one `Esc` asks for the run's stop, D takes it exactly once.
void stepStop(Harness& h) {
    phase("5. stop mid-stream");
    h.pressKey(ImGuiKey_Escape);
    h.idle(1);

    check(h.core->cancelRequested, "stop: Esc asked for the run to stop (the UI latch)");
    check(tuiTakeCancel(h.handle.get()) == 1, "stop: D takes the request (the /stop path)");
    check(tuiTakeCancel(h.handle.get()) == 0, "stop: a second take finds nothing (consume-once)");

    // D's half: the stream ends and the run is ready again; the partial answer is the answer, so
    // the next step stores it as the run's final message.
    tuiStreamChatMessageClear(h.handle.get());
    tuiReadyStatus(h.handle.get(), 1);
    h.idle(2);

    check(!h.core->cancelRequested, "stop: the latch clears when the run ends");
    check(!h.core->chat.hasStreamMsg, "stop: the live row is gone with the run");
}

/// Step 6 - answer: the run's final message becomes the thread's newest block.
void stepAnswer(Harness& h) {
    phase("6. answer");
    addMessage(h, TuiChatMessageType_FinalAnswer, "[4] plan",
               "the plan is two steps: read, then build");
    h.idle(2);

    check(h.core->chat.outputLines.size() >= 3, "answer: the answer joined the store");
    check(h.core->ui.view == model::ThreadView::Ready, "answer: the thread is the body again");
    if (h.text()) {
        check(h.last.hasText("read, then build"), "answer: the answer's text is drawn");
        check(h.last.hasText("Send"), "answer: the composer is back to Send");
    }
}

/// Step 7 - copy: the palette's copy row puts the newest answer on the clipboard.
void stepCopy(Harness& h) {
    phase("7. copy");
    // An empty clipboard first: the check below must not pass on a value an earlier frame left.
    ImGui::SetClipboardText("");
    g_clipboard.clear();
    h.pressCtrl(ImGuiKey_K);
    h.idle(1);
    check(h.core->palette.open, "copy: Ctrl+K opens the palette");
    check(selectPaletteRow(h, "actions.copyLastAnswer"), "copy: the copy row is listed");
    h.pressKey(ImGuiKey_Enter);
    h.idle(1);

    const std::string copied = readClipboard(h);
    check(!copied.empty(), "copy: the row wrote to the clipboard");
    check(copied == "the plan is two steps: read, then build",
          "copy: the clipboard holds the answer's body, not its header (" + quoted(copied) + ")");
    check(!h.core->palette.open, "copy: activating a row closes the palette");
}

/// Step 8 - agents tab: the pipeline mirror feeds the lanes, the inspector shows them, and the
/// rows say the state the run is in.
void stepAgentsTab(Harness& h) {
    phase("8. agents tab");
    // A second run: the pipeline's updates only mean "running" while one is in flight, and a lane
    // whose run ended without finishing it is `failed` by design (`settleAgentLanes`).
    tuiReadyStatus(h.handle.get(), 0);
    updateAgent(h, "planner", "assistant", "I will read the tree first", "read_file", 4200);
    updateAgent(h, "coder", "assistant", "writing the patch", "", 5100);
    h.idle(2);
    check(h.core->ui.agents.size() == 2, "agents: the ABI's updates reached the lane list");
    // The ABI's numbers land on the lane even where the row has no room to print them (the panel
    // is 30 cells wide and the state word is reserved first, D-BLOCK). Guarded: a missed lane must
    // read as a second failure, not as an out-of-bounds read.
    if (h.core->ui.agents.size() >= 2) {
        check(h.core->ui.agents[0].currentTool == "read_file",
              "agents: the ABI's current tool reached the lane (" +
                  h.core->ui.agents[0].currentTool + ")");
        check(h.core->ui.agents[0].tokens == 4200,
              "agents: the ABI's context count reached the lane");
    }

    // The panel is docked at the reference width (the `-1 auto` rule), so it is already up; the
    // palette row is only needed where the width closed it, and it is the grid's documented entry
    // point for that case (`Ctrl+I` is not delivered by a terminal, plan/key_gate.md §3b).
    check(h.core->inspector.open, "agents: the inspector is docked at this width");
    if (!h.core->inspector.open) {
        check(runPaletteRow(h, "actions.toggleInspector"),
              "agents: the inspector opens from the palette");
        h.idle(2);
        check(h.core->inspector.open, "agents: the panel has an area this frame");
    }

    // The tab: a click on the strip in the grid, `Ctrl+5` in the pixel profile (the chord the
    // terminal swallows). Both write `ShellPrefs::tab` through the same answer.
    if (h.text()) {
        check(h.clickOn("Agents"), "agents: the Agents tab is on screen to click");
    } else {
        h.pressCtrl(ImGuiKey_5);
    }
    h.idle(2);

    check(h.core->ui.prefs.tab == model::InspectorTab::Agents,
          "agents: the Agents tab is selected");
    check(h.core->inspector.tab == model::InspectorTab::Agents, "agents: the frame drew that tab");
    if (h.text()) {
        check(h.last.hasText("planner"), "agents: the first lane's row is drawn");
        check(h.last.hasText("coder"), "agents: the second lane is on the mission strip");
        check(h.last.hasText("running"), "agents: the lane says what state it is in");
    }
}

/// Step 9 - activity tab: the record stream becomes the user-facing history, and the developer
/// levels stay out of it.
void stepActivityTab(Harness& h) {
    phase("9. activity tab");
    // The run the lanes belonged to is over: an `idle` run is what the remaining steps
    // (settings, quit) expect, and a quit chord while a run is active would only arm.
    tuiReadyStatus(h.handle.get(), 1);
    // Short messages: the Activity row's message column is a narrow one (the panel is 30 cells),
    // so a long line is cut with an ellipsis and a check on the full text would be a check on
    // the column's width instead of on the projection.
    addLogRecord(h, TuiLogLevel_Info, "loaded");
    addLogRecord(h, TuiLogLevel_Warn, "slow");
    addLogRecord(h, TuiLogLevel_Debug, "dvbg");
    h.idle(2);

    check(!h.core->ui.activity.empty(), "activity: the records reached the projection");
    check(h.core->ui.unreadActivity > 0, "activity: arrivals off screen are unread");

    if (h.text()) {
        check(h.clickOn("Activity"), "activity: the Activity tab is on screen to click");
    } else {
        h.pressCtrl(ImGuiKey_6);
    }
    h.idle(2);

    check(h.core->ui.prefs.tab == model::InspectorTab::Activity,
          "activity: the Activity tab is selected");
    check(h.core->ui.unreadActivity == 0, "activity: showing the tab is reading it");
    if (h.text()) {
        check(h.last.hasText("loaded"), "activity: the info record is drawn");
        check(h.last.hasText("slow"), "activity: the warn record is drawn");
        check(!h.last.hasText("dvbg"),
              "activity: a debug record never reaches the user-facing list (D-ACTIVITY)");
    }
}

/// Step 10 - raw log open/close: the typed command opens the developer surface, `Esc` closes it.
///
/// The typed path is the composer's own: a `/` on an empty line is the palette's COMMANDS entry
/// point (design §4.6), the rest of the line narrows the list to the `/rawlog` row, and `Enter`
/// submits it - the palette's `submitLine()`, which runs the same intercept a typed line does.
/// So the step proves the `/` rule, the command row and the surface's flag in one gesture, and it
/// is the gesture the grid's user actually has (`Ctrl+Shift+L` is not delivered by a terminal).
void stepRawLog(Harness& h) {
    phase("10. raw log open/close");
    if (h.text()) {
        check(h.clickOn("Ask anything"), "rawlog: the composer's field is reachable");
        h.idle(1);
        h.typeText("/");
        h.idle(1);
        check(h.core->palette.open, "rawlog: a leading / opens the palette at its commands");
        h.typeText("rawlog");
        h.idle(1);
        check(selectPaletteRow(h, "commands.rawlog"), "rawlog: the command row is listed");
    } else {
        // The pixel profile has the same typed command; there is no composer field to type into
        // here, so the palette is opened first and the line is its own query.
        h.pressCtrl(ImGuiKey_K);
        h.idle(1);
        check(h.core->palette.open, "rawlog: Ctrl+K opens the palette");
        h.typeText("/rawlog");
        check(selectPaletteRow(h, "ask.freeText"), "rawlog: the free-text row is listed");
    }
    h.pressKey(ImGuiKey_Enter);
    h.idle(2);

    check(h.core->ui.rawLogOpen, "rawlog: the command opened the surface");
    check(tuiIsRawLogOpen(h.handle.get()) == 1,
          "rawlog: the ABI reports it open (D's logger poll)");
    if (h.text()) {
        check(h.last.hasText(views::kRawLogTitle), "rawlog: the overlay is drawn");
        h.pressKey(ImGuiKey_Escape);
        h.idle(2);
        check(!h.core->ui.rawLogOpen,
              "rawlog: Esc closed the surface (the Esc chain's first step)");
    } else {
        // The pixel profile's Raw Log is the *drawer* - part of the tiling, deliberately not an
        // `Esc` step (D-OVERLAY: a drawer leaves the composer usable) - so it is closed with its
        // own chord, which is the one the terminal cannot deliver.
        h.pressCtrlShift(ImGuiKey_L);
        h.idle(2);
        check(!h.core->ui.rawLogOpen, "rawlog: Ctrl+Shift+L closed the drawer");
    }
    check(tuiIsRawLogOpen(h.handle.get()) == 0, "rawlog: the ABI reports it closed");
}

/// Step 11 - settings change: a persisted preference moves through the sheet.
void stepSettings(Harness& h) {
    phase("11. settings change");
    check(runPaletteRow(h, "actions.openSettings"), "settings: the sheet opens from the palette");
    h.idle(2);
    check(h.core->settingsSheet.open, "settings: the sheet is up");

    // Walk to the Raw Log level row with the sheet's own cursor key, then activate it. `Ctrl+N`
    // also releases the search field, which is what makes the following `Enter` the list's (the
    // sheet's documented rule: `Enter` types into the field, a cursor key means "into the list").
    int guard = 0;
    bool onRow = false;
    while (guard++ < 24) {
        const std::vector<views::SettingsRow>& rows = h.core->settingsSheet.rows;
        const int selected = h.core->settingsSheet.selected;
        if (selected >= 0 && static_cast<std::size_t>(selected) < rows.size() &&
            std::strcmp(rows[static_cast<std::size_t>(selected)].id, views::kSettingsRowRawLog) ==
                0) {
            onRow = true;
            break;
        }
        h.pressCtrl(ImGuiKey_N);
        h.idle(1);
    }
    check(onRow, "settings: the Raw Log level row is reachable from the keyboard");

    const int before = h.core->ui.prefs.rawLog;
    h.pressKey(ImGuiKey_Enter);
    h.idle(2);
    check(h.core->ui.prefs.rawLog != before, "settings: activating the row moved the value (" +
                                                 std::to_string(before) + " -> " +
                                                 std::to_string(h.core->ui.prefs.rawLog) + ")");
    check(h.core->ui.prefs.rawLog == model::kRawLogRaw,
          "settings: off cycles to the first open level (raw)");

    h.pressKey(ImGuiKey_Escape);
    h.idle(2);
    check(!h.core->settingsSheet.open, "settings: Esc closes the sheet");
}

/// Step 12 - quit: the chord asks, the frame answers "user terminated", and the changed
/// preference is on disk.
void stepQuit(Harness& h) {
    phase("12. quit");
    check(!h.quit, "quit: the frame loop is still running");
    // The frames are driven one at a time here: the flag is set by the frame that sees the chord
    // and answered by the *next* one, and a `pressCtrl()` would run past both.
    h.frame([](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Ctrl, true); });
    h.frame([](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Q, true); });
    check(h.core->quitRequested, "quit: Ctrl+Q asked the loop to end");
    check(!h.quit, "quit: the request is a flag, answered by the next frame");

    h.frame([](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Q, false); });
    check(h.quit, "quit: the frame answered 'user terminated'");
    if (h.text()) {
        check(fileContains("parity_scratch/ui.conf", "UiLog=raw"),
              "quit: the changed preference reached ui.conf on the quit path (D-UI-CONF)");
        check(fileContains("parity_scratch/ui.conf", "Version=1"),
              "quit: the file carries the format version");
    }
}

void runCoreLoop(Harness& h) {
    stepStart(h);
    stepSelectSession(h);
    stepAsk(h);
    stepStream(h);
    stepStop(h);
    stepAnswer(h);
    stepCopy(h);
    stepAgentsTab(h);
    stepActivityTab(h);
    stepRawLog(h);
    stepSettings(h);
    stepQuit(h);
}

// ---------------------------------------------------------------------------
// Goldens: the whole shell at the four reference sizes
// ---------------------------------------------------------------------------

/// The golden scene: the final anatomy, with nothing that depends on the clock.
///
/// * three sessions with `updatedAtMs == 0`, so the rail is the flat `SESSIONS` list and every
///   row's time label is empty (a relative label would change across a day, and a golden that
///   expires is not a golden);
/// * a two-message thread whose summaries carry no `[1.2s]` suffix, so no block prints a
///   duration;
/// * the default status sentence (`ctx 0/0 | - | none | Ready`) and an idle run, so neither the
///   now bar nor the top bar has a clock on it;
/// * the composer empty with the field released (`Esc`), so no caret blinks in the capture.
void buildGoldenScene(Harness& h) {
    std::vector<SessionItem> sessions;
    sessions.push_back(
        makeSession("20260815-100000-aaaa", "Alpha session", "first user preview", 3, 1, 0));
    sessions.push_back(
        makeSession("20260815-100000-bbbb", "Beta session", "second user preview", 5, 0, 0));
    sessions.push_back(makeSession("20260815-100000-cccc", "Gamma session", "", 0, 0, 0));
    setSessions(h, sessions);

    addMessage(h, TuiChatMessageType_User, "[1] build the project", "build the project");
    addMessage(h, TuiChatMessageType_FinalAnswer, "[2] plan", "read, then build");
}

std::string goldenText(Harness& h, int w, int ht, const char* name = "parity-shell") {
    g_w = w;
    g_h = ht;
    h.idle(1); // the layout of the new size
    h.parkPointer();
    h.idle(1); // the frame the pointer did not disturb
    const Grid& grid = h.last;
    if (g_print) {
        // A golden that lost its fills is the one failure a text diff makes hard to see (the
        // glyphs are identical), so the capture prints a few cells' background indices.
        std::fprintf(stderr, "golden %dx%d: bg(0,0)=%d bg(2,2)=%d bg(40,10)=%d bg(mid)=%d\n", w, ht,
                     static_cast<int>(grid.bgAt(0, 0)), static_cast<int>(grid.bgAt(2, 2)),
                     static_cast<int>(grid.bgAt(40, 10)),
                     static_cast<int>(grid.bgAt(grid.nx / 2, grid.ny / 2)));
    }

    std::string text = std::string("name: ") + name + "\n";
    text += "screen: " + std::to_string(w) + "x" + std::to_string(ht) + "\n";
    text += "renderer: workbench (the only renderer since task 41)\n";
    text += "glyphs: unicode\n";
    text += "depth: truecolor\n";
    text += "text:\n";
    for (int y = 0; y < grid.ny; ++y) {
        std::string row = grid.rowText(y);
        while (!row.empty() && row.back() == ' ')
            row.pop_back();
        text += row;
        text += '\n';
    }
    // Half of what a container is: the painted background, as runs of one colour index.
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

/// The reference sizes of D-GOLDEN, in the order `plan/baseline.md` lists them.
struct GoldenSize {
    int w;
    int h;
};

const GoldenSize kGoldenSizes[] = {{120, 36}, {100, 30}, {80, 24}, {60, 20}};
constexpr std::size_t kGoldenSizeCount = sizeof(kGoldenSizes) / sizeof(kGoldenSizes[0]);

std::string goldenPath(int w, int h) {
    return std::string(TEST_PARITY_GOLDEN_DIR) + "/parity-shell-" + std::to_string(w) + "x" +
           std::to_string(h) + ".txt";
}

/// One golden, compared or written: the verdict the two scenes below share.
void goldenVerdict(const std::string& path, const std::string& label, const std::string& text,
                   bool update, const Grid* screen) {
    if (update) {
        check(writeFileText(path, text), "golden written: " + path);
        return;
    }
    bool ok = false;
    const std::string expected = readFileText(path, ok);
    if (!ok) {
        check(false, "golden is readable: " + path + " (run with --update to create it)");
        return;
    }
    if (expected == text) {
        check(true, "the " + label + " golden matches");
        return;
    }
    std::size_t line = 1;
    std::size_t at = 0;
    while (at < expected.size() && at < text.size() && expected[at] == text[at]) {
        line += expected[at] == '\n' ? 1 : 0;
        ++at;
    }
    check(false, "the " + label + " golden matches (first difference on line " +
                     std::to_string(line) + ": " + path + ")");
    if (g_verbose && screen != nullptr)
        std::fprintf(stderr, "--- screen %s\n%s", label.c_str(), screen->dump().c_str());
}

void checkGoldens(bool update) {
    phase("goldens");
    for (std::size_t i = 0; i < kGoldenSizeCount; ++i) {
        const GoldenSize size = kGoldenSizes[i];
        const std::string label = std::to_string(size.w) + "x" + std::to_string(size.h);
        // A fresh state per size: the scene is a function of the size alone, so a leftover scroll
        // offset or focus latch from the size before it cannot reach the next golden.
        Harness h;
        harnessInitText(h);
        buildGoldenScene(h);
        h.idle(2);                   // the thread's first-open catch-up needs its second frame
        h.pressKey(ImGuiKey_Escape); // release the composer's field: no blinking caret
        h.idle(1);
        check(!h.core->composer.focused, "golden " + label + ": the field is released");

        const std::string text = goldenText(h, size.w, size.h);
        goldenVerdict(goldenPath(size.w, size.h), label + " shell", text, update, &h.last);
        harnessShutdownText(h);
    }
    // Leave the shared display size as the harness expects it.
    g_w = 120;
    g_h = 36;
}

/// The narrow band's second shape: the rail as a **panel** (design §3.3's "single column, both
/// overlays"), at 60x20.
///
/// This is the shape task 40's first cut blessed while it was *broken*: the panel was drawn before
/// the thread, whose content lives in a child window - and a child's draw list is emitted after the
/// parent's, so the thread's glyphs came through the panel's fill (`views/shell.cpp`'s paint-order
/// note; the fix draws the panel at the end of the frame, with the floating inspector). The checks
/// below are the property and the golden is the picture:
///
///   * the first `Ctrl+B` on a narrow display **opens** the panel: the preference is `-1 auto`,
///     which the shell resolves to "closed" here, so a toggle that read the preference would write
///     a second "closed" and do nothing visible (the inspector's `visible` rule, task 29);
///   * the panel is **opaque**: the thread behind it is not on screen while the panel's own rows
///     are (the defect was exactly the opposite);
///   * `/` still reaches the panel's search field: a panel is not a modal surface, so the
///     "does `Esc` close it" answer must not gate its keys;
///   * one `Esc` closes it - the layout has carried `OverlayLayer::Rail` since task 17 for this -
///     and the thread comes back.
void checkNarrowRailPanel(bool update) {
    phase("narrow rail panel");
    Harness h;
    harnessInitText(h);
    g_w = 60;
    g_h = 20;
    buildGoldenScene(h);
    h.idle(2);
    h.pressKey(ImGuiKey_Escape); // release the composer's field
    h.idle(1);

    // Premises: the panel band, an automatic rail, and no rail on screen.
    check(h.core->ui.prefs.railOpen == -1, "rail panel: the preference is still auto");
    check(!h.core->ui.rail.open, "rail panel: an automatic rail is closed at 60x20");
    check(!h.last.hasText("Alpha session"), "rail panel: ...and its rows are not drawn");

    h.pressCtrl(ImGuiKey_B);
    h.idle(3);
    check(h.core->ui.prefs.railOpen == 1,
          "rail panel: the first Ctrl+B opens it (the visible-state toggle)");
    check(h.core->ui.rail.open && h.core->ui.rail.overlay, "rail panel: it is the panel shape");
    check(h.last.hasText("Alpha session"), "rail panel: its rows are drawn");
    check(h.last.hasText("Search"), "rail panel: its search field is drawn");
    check(!h.last.hasText("build the project"),
          "rail panel: it is opaque - the thread behind it is not visible");

    const std::string text = goldenText(h, 60, 20);
    goldenVerdict(std::string(TEST_PARITY_GOLDEN_DIR) + "/parity-rail-panel-60x20.txt",
                  "60x20 rail panel", text, update, &h.last);

    // The panel's keyboard entry, the shape this fix restores. The rail reads printable keys
    // from the character queue (`ui::takeCharacter`), so `typeText()` is the press a terminal
    // delivers - `pressKey()` only sets a key state and would never reach it. `filterFocus` is
    // a one-frame request (the field consumes it on the next frame), so it is read here.
    h.typeText("/");
    check(h.core->ui.rail.filterFocus, "rail panel: `/` asks for its search field");
    h.idle(1);
    h.typeText("alp");
    h.idle(1);
    // The rail's own `Esc` step comes before the frame's "close the panel": one press clears
    // the query (D-ESC), the next one closes the panel.
    h.pressKey(ImGuiKey_Escape);
    h.idle(2);
    check(h.core->ui.rail.overlay, "rail panel: Esc clears the filter before it closes the panel");
    check(h.core->ui.rail.filterBuf[0] == '\0', "rail panel: ...and the query is gone");

    h.pressKey(ImGuiKey_Escape);
    h.idle(2);
    check(!h.core->ui.rail.overlay, "rail panel: Esc closes the panel");
    check(h.core->ui.prefs.railOpen == 0, "rail panel: ...as an explicit choice");
    check(h.last.hasText("build the project"), "rail panel: the thread is back");
    harnessShutdownText(h);
    g_w = 120;
    g_h = 36;
}

/// Focus mode (task 43): the mode's picture at 120x36, plus the two halves of its contract that
/// only a frame can show - the chrome goes, the state stays.
///
/// The ordinary `parity-shell-120x36` golden is this scene's "out" picture: both are captured from
/// the same fixture, and the checks below prove the mode is reversible (`Esc` puts the rail and the
/// bar back) - which is the property a second identical golden would not add.
void checkFocusMode(bool update) {
    phase("focus mode");
    g_w = 120;
    g_h = 36;
    Harness h;
    harnessInitText(h);
    buildGoldenScene(h);
    h.idle(2);                   // the thread's first-open catch-up needs its second frame
    h.pressKey(ImGuiKey_Escape); // release the composer's field: no blinking caret
    h.idle(1);
    check(!h.core->ui.focusMode, "focus: the mode starts off");

    // Premises from the ordinary tiling: the rail's header and the bar's title are on screen, and
    // the transcript starts where the rail ends.
    int x = 0, y = 0;
    check(h.last.findText("SESSIONS", x, y), "focus premise: the rail draws its header");
    check(h.last.findText("Alpha session", x, y),
          "focus premise: the bar and the rail carry the title");
    check(h.last.findText("YOU", x, y) && x > 20,
          "focus premise: the transcript starts after the rail (cell " + std::to_string(x) + ")");

    // In: `F11` is the design's chord. A function key types nothing, so the keymap's text-field
    // rule lets it through even while the composer holds the keyboard (the terminal's own path is
    // the palette's row - no terminal delivers `F11`).
    h.pressKey(ImGuiKey_F11);
    h.idle(2);
    check(h.core->ui.focusMode, "focus: F11 turns the mode on");
    check(!h.last.hasText("SESSIONS"), "focus: the rail is hidden");
    check(!h.last.hasText("Alpha session"), "focus: the top bar is hidden with it");
    check(h.last.hasText("read, then build"), "focus: the thread is still drawn");
    check(h.last.hasText("Ask anything..."), "focus: the composer is untouched");
    check(h.last.hasText("Ctrl+K commands") && h.last.hasText("Ready"),
          "focus: the now bar still carries the state (NFR-16)");
    check(h.last.findText("YOU", x, y) && x == 16,
          "focus: the transcript is the centred 90-cell column (YOU at cell " + std::to_string(x) +
              ")");

    const std::string text = goldenText(h, 120, 36, "parity-focus");
    goldenVerdict(std::string(TEST_PARITY_GOLDEN_DIR) + "/parity-focus-120x36.txt",
                  "120x36 focus mode", text, update, &h.last);

    // Out: one `Esc`, the mode's own step - and the picture is the ordinary one again.
    h.pressKey(ImGuiKey_Escape);
    h.idle(2);
    check(!h.core->ui.focusMode, "focus: Esc leaves the mode");
    check(h.last.hasText("SESSIONS"), "focus: the rail is back");
    check(h.last.hasText("Alpha session"), "focus: the bar is back");
    check(h.last.findText("YOU", x, y) && x > 20,
          "focus: the transcript is back beside the rail (cell " + std::to_string(x) + ")");

    harnessShutdownText(h);
    // Leave the shared display size as the harness expects it.
    g_w = 120;
    g_h = 36;
}

// ---------------------------------------------------------------------------
// The perf gate
// ---------------------------------------------------------------------------

/// The fixture the design names: 1000 messages, 2000 sessions, 5000 raw records - and the 500
/// Activity rows the projection keeps out of those same arrivals, so one write path fills both.
void buildPerfFixture(Harness& h) {
    // The strings have to point at live memory during the snapshot call (it copies them), so the
    // storage is built first and indexed.
    std::vector<std::string> ids;
    std::vector<std::string> titles;
    ids.reserve(2000);
    titles.reserve(2000);
    std::vector<SessionItem> sessions;
    sessions.reserve(2000);
    for (int i = 0; i < 2000; ++i) {
        ids.push_back("perf-session-" + std::to_string(i));
        titles.push_back("Perf session " + std::to_string(i));
    }
    for (int i = 0; i < 2000; ++i) {
        sessions.push_back(makeSession(ids[static_cast<std::size_t>(i)].c_str(),
                                       titles[static_cast<std::size_t>(i)].c_str(), "", 2,
                                       i == 0 ? 1 : 0, 0));
    }
    setSessions(h, sessions);

    for (int i = 0; i < 1000; ++i) {
        const std::string body =
            "message " + std::to_string(i) + ": **bold** and a list\n- one\n- two";
        addMessage(h, i % 10 == 0 ? TuiChatMessageType_User : TuiChatMessageType_Assistant,
                   "[" + std::to_string(i) + "] seeded", body);
    }
    for (int i = 0; i < 5000; ++i) {
        addLogRecord(h, (i % 7 == 0) ? TuiLogLevel_Warn : TuiLogLevel_Info,
                     "record " + std::to_string(i), "detail line", "perf");
    }
}

/// Wall-clock milliseconds: the perf gate's only clock.
double nowMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

double median(std::vector<double> values) {
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

/// One measurement: build the fixture, render `frames` frames, answer the p50 frame time and the
/// session-open time (state + fixture + first frame).
struct PerfRun {
    double openMs = 0.0;
    double frameMs = 0.0;
};

PerfRun measureWorkbench(int frames) {
    PerfRun run;
    const double started = nowMs();
    Harness h;
    harnessInitText(h);
    buildPerfFixture(h);
    h.idle(1); // the first frame: state + fixture + one render
    run.openMs = nowMs() - started;

    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        const double before = nowMs();
        h.frame();
        samples.push_back(nowMs() - before);
    }
    run.frameMs = median(samples);
    // The bounded rendering is what makes the fixture affordable: a window over the last groups, a
    // bounded rail viewport, a bounded Activity ring - never the whole store. The bounds are the
    // *asserted* numbers, so a regression that starts drawing the store fails here rather than only
    // showing up in the timing (review finding 3). 41 = the 40-group window plus the live answer's
    // row; 15 = the rail's 13 two-line slots plus one row of overscan and one of slack.
    const std::size_t groups = h.core->threadAdapter.groupCount();
    check(h.core->ui.thread.drawnGroups <= 41,
          "perf: the thread drew its bounded window, not the store (" +
              std::to_string(h.core->ui.thread.drawnGroups) + " of " + std::to_string(groups) +
              " groups)");
    check(groups > 41, "perf: the fixture really has more groups than the window (" +
                           std::to_string(groups) + ")");
    check(h.core->ui.rail.drawnRows <= 15, "perf: the rail drew its viewport, not 2000 rows (" +
                                               std::to_string(h.core->ui.rail.drawnRows) +
                                               " rows)");
    check(h.core->ui.activity.size() <= 500,
          "perf: the Activity projection kept its 500-row bound (" +
              std::to_string(h.core->ui.activity.size()) + " rows)");
    harnessShutdownText(h);
    return run;
}

/// `plan/baseline.md`'s numbers for the same per-frame work: a 1000-message seed at 16.70 ms per
/// frame (`--frames 600` p50 over 10021 ms) and 37 ms session open (`--frames 1`). The baseline
/// was measured on a PTY through the ncurses backend, so these are a *budget* for a headless
/// run: a headless frame slower than the terminal's would be a regression with the terminal's
/// cost still to come.
constexpr double kBaselineFrameMs = 16.70;
constexpr double kBaselineOpenMs = 37.0;
/// The last measurement of this fixture on this harness (task 40): the workbench's own p50
/// frame time and session open, recorded beside the legacy renderer's numbers on the same run.
/// The legacy half cannot be measured any more - its renderer is gone (task 41) - so the
/// workbench's recording is what the gate compares against.
constexpr double kRecordedFrameMs = 0.53;
constexpr double kRecordedOpenMs = 8.2;
/// The design's allowance: "within 10 % of plan/baseline.md".
constexpr double kPerfAllowance = 1.10;

void checkPerf() {
    phase("perf gate");
    constexpr int kFrames = 40;
    // Warm-up pass, not gated (task 41): task 40 measured the workbench *after* the other
    // renderer's run in the same process, so its 8.2 ms session open carried a warm allocator,
    // atlas and page cache. That renderer is gone, and a cold single run pays ~1.5 ms more for
    // the same work - a cost of being first, not of the renderer. Re-recording the baseline to
    // the cold number would make the gate pass whatever the next regression is, so the warm-up
    // pass restores the shape the numbers were taken in and `kRecordedOpenMs` stays the gate.
    const PerfRun warmUp = measureWorkbench(kFrames);
    // The first run's numbers are printed and deliberately not gated: it is the first run in the
    // process (atlas, allocator, page cache), and `plan/baseline.md` records the cold open beside
    // the warm number so the cost of being first stays visible instead of being folded into a
    // budget. One line for one measurement: the frame time and the session open are the same
    // run's, not two samples.
    std::printf("perf: first run p50 frame %s ms, session open %s ms (cold, not gated)\n",
                num(warmUp.frameMs).c_str(), num(warmUp.openMs).c_str());
    // Three measured runs, the median gated: one 40-frame sample on a shared machine moves by
    // ±10 % on its own, which would fail the gate on noise rather than on a regression. The
    // median of three medians is the number the log and the gate use.
    std::vector<double> frameRuns;
    std::vector<double> openRuns;
    for (int run = 0; run < 3; ++run) {
        const PerfRun sample = measureWorkbench(kFrames);
        frameRuns.push_back(sample.frameMs);
        openRuns.push_back(sample.openMs);
    }
    const PerfRun workbench{median(openRuns), median(frameRuns)};
    std::printf("perf: samples   frames %s / %s / %s ms, opens %s / %s / %s ms\n",
                num(frameRuns[0]).c_str(), num(frameRuns[1]).c_str(), num(frameRuns[2]).c_str(),
                num(openRuns[0]).c_str(), num(openRuns[1]).c_str(), num(openRuns[2]).c_str());

    std::printf("perf: workbench p50 frame %s ms (median of 3), session open %s ms\n",
                num(workbench.frameMs).c_str(), num(workbench.openMs).c_str());
    std::printf("perf: recorded  p50 frame %s ms, session open %s ms (pre-removal, task 40)\n",
                num(kRecordedFrameMs).c_str(), num(kRecordedOpenMs).c_str());
    std::printf("perf: budget    p50 frame <= %s ms, session open <= %s ms (recorded + 10%%)\n",
                num(kRecordedFrameMs * kPerfAllowance).c_str(),
                num(kRecordedOpenMs * kPerfAllowance).c_str());

    // The regression clause: the recorded numbers are the last measurement of this same fixture
    // on this same harness (task 40 ran both renderers side by side; task 41 removed the other
    // one, so the workbench's own recording is the baseline).
    // No slack beyond the design's allowance: the margin was ~1.8x, and half a millisecond of
    // slack on a 0.95 ms baseline would quietly permit a 58 % regression (review finding 3).
    check(workbench.frameMs <= kRecordedFrameMs * kPerfAllowance,
          "perf: the frame is within 10 % of the recorded baseline (" + num(workbench.frameMs) +
              " vs " + num(kRecordedFrameMs) + " ms)");
    check(workbench.openMs <= kRecordedOpenMs * kPerfAllowance,
          "perf: session open is within 10 % of the recorded baseline (" + num(workbench.openMs) +
              " vs " + num(kRecordedOpenMs) + " ms)");
    // The absolute clause: the budget `plan/baseline.md` recorded for the same per-frame work.
    check(workbench.frameMs <= kBaselineFrameMs * kPerfAllowance,
          "perf: the frame is inside plan/baseline.md's budget (" + num(workbench.frameMs) +
              " <= " + num(kBaselineFrameMs * kPerfAllowance) + " ms)");
    check(workbench.openMs <= kBaselineOpenMs * kPerfAllowance,
          "perf: session open is inside plan/baseline.md's budget (" + num(workbench.openMs) +
              " <= " + num(kBaselineOpenMs * kPerfAllowance) + " ms)");
}

// ---------------------------------------------------------------------------
// The GUI run: the same loop, plus the pixel profile's own evidence
// ---------------------------------------------------------------------------

int runGui(const std::string& outDir) {
    {
        phase("gui: start");
        Harness h;
        harnessInitGui(h, outDir + "/parity-gui.ini");
        h.idle(3);
        tui::GuiWindowGeometry geometry{};
        check(h.core->backend->guiWindowGeometry(geometry), "gui: the backend reports its surface");
        check(geometry.width == 1280 && geometry.height == 820,
              "gui: the window is the design's 1280x820 (" + std::to_string(geometry.width) + "x" +
                  std::to_string(geometry.height) + ")");

        runCoreLoop(h);

        // The one capture: the frame the loop ended on, written as the pixel profile's golden
        // (Q-05: a PPM plus a recorded review, not a text golden - a pixel frame is not
        // comparable across drivers).
        phase("gui: capture");
        const std::string path = outDir + "/gui-1280x820-parity.ppm";
        if (!h.core->backend->captureNextFramePpm(path)) {
            check(false, "gui: the backend accepted the capture request");
        } else {
            h.idle(1);
            std::ifstream in(path, std::ios::binary);
            std::string magic;
            int w = 0, ht = 0, maxv = 0;
            if (in.good())
                in >> magic >> w >> ht >> maxv;
            check(magic == "P6" && w == 1280 && ht == 820,
                  "gui: the capture is a 1280x820 P6 PPM (header '" + magic + "', " +
                      std::to_string(w) + "x" + std::to_string(ht) + ", " + path + ")");
        }
        harnessShutdownGui(h);
    }
    {
        // The focus-mode capture (task 43): its own pass, because the loop's harness is already
        // past its quit handshake - and `tuiRender()` draws nothing once `quitRequested` is set, so
        // the mode has to be entered and captured in a frame that is still live.
        phase("gui: focus capture");
        Harness h;
        harnessInitGui(h, outDir + "/parity-gui-focus.ini");
        h.idle(3);
        buildGoldenScene(h);
        h.idle(2);
        h.pressKey(ImGuiKey_Escape); // release the composer's field
        h.idle(1);
        check(!h.core->ui.focusMode, "gui focus: the mode starts off");
        h.pressKey(ImGuiKey_F11);
        h.idle(2);
        check(h.core->ui.focusMode, "gui focus: F11 turns focus mode on");
        tui::GuiWindowGeometry geometry{};
        check(h.core->backend->guiWindowGeometry(geometry) && geometry.width == 1280,
              "gui focus: the window is still the design's 1280x820 (" +
                  std::to_string(geometry.width) + "x" + std::to_string(geometry.height) + ")");
        const std::string path = outDir + "/gui-1280x820-focus.ppm";
        if (!h.core->backend->captureNextFramePpm(path)) {
            check(false, "gui focus: the backend accepted the capture request");
        } else {
            h.idle(1);
            std::ifstream in(path, std::ios::binary);
            std::string magic;
            int w = 0, ht = 0, maxv = 0;
            if (in.good())
                in >> magic >> w >> ht >> maxv;
            check(magic == "P6" && w == 1280 && ht == 820,
                  "gui focus: the capture is a 1280x820 P6 PPM (header '" + magic + "', " +
                      std::to_string(w) + "x" + std::to_string(ht) + ", " + path + ")");
        }
        h.pressKey(ImGuiKey_Escape);
        h.idle(2);
        check(!h.core->ui.focusMode, "gui focus: Esc leaves it again");
        harnessShutdownGui(h);
    }
    return 0;
}

int runText(bool update) {
    resetScratchDir("parity_scratch");
    {
        // The loop runs on its own state: the settings and quit steps write `ui.conf` into the
        // scratch directory, and the goldens and the perf gate build theirs from scratch.
        phase("loop: init");
        Harness h;
        harnessInitText(h);
        h.core->iniFilename = "parity_scratch/parity.ini";
        runCoreLoop(h);
        harnessShutdownText(h);
    }
    checkGoldens(update);
    checkNarrowRailPanel(update);
    checkFocusMode(update);
    checkPerf();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    bool update = false;
    bool gui = false;
    std::string outDir;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-v" || arg == "--verbose")
            g_verbose = true;
        else if (arg == "--print")
            g_print = true;
        else if (arg == "--update")
            update = true;
        else if (arg == "--gui")
            gui = true;
        else if (arg == "--out" && i + 1 < argc)
            outDir = argv[++i];
        else if (arg == "-h" || arg == "--help")
            usage(argv[0], "runs the core loop; --gui drives the pixel backend under xvfb");
        else
            usage(argv[0], "unknown argument: " + arg);
    }
    if (gui && outDir.empty())
        usage(argv[0], "--gui needs --out DIR for the capture");
    if (!gui && !outDir.empty())
        usage(argv[0], "--out belongs to --gui");
    if (gui && update)
        usage(argv[0], "--update rewrites the text goldens and has no meaning with --gui");

    // The capture writes into `--out`, and every other GUI harness in this suite refuses a missing
    // directory *before* the backend starts (the suite's `SETUP [arguments]` wording). Doing it
    // here keeps a typo from costing the whole loop and then reporting an unread PPM header as a
    // check failure (review finding 2).
    if (gui && ::mkdir(outDir.c_str(), 0755) != 0 && errno != EEXIST) {
        std::fprintf(stderr,
                     "SETUP [arguments]: the output directory is missing or not writable: %s\n",
                     outDir.c_str());
        return 2;
    }

    if (gui)
        runGui(outDir);
    else
        runText(update);

    if (g_failures == 0) {
        std::printf("parity_core_loop: %d checks passed (%d frames, %s)\n", g_checks, g_frames,
                    gui ? "gui" : "text grid");
        return 0;
    }
    std::fprintf(stderr, "parity_core_loop: %d of %d checks failed\n", g_failures, g_checks);
    return 1;
}
