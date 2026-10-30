// test_settings_sheet.cpp
//
// The minimal settings sheet (views/settings.h + model/settings.h, design §4.10 / FR-17 /
// task 34): the three settings the MVP changes, the diagnostics paths, and the wiring that
// makes the sheet reachable and closable.
//
// What this binary proves, in order:
//
//   1. the source. The view writes no colour literal, reads no display size, names no v4 type and
//      no `tui.h`, and keeps its vocabulary in the header; it never touches a file (the frame owns
//      both write paths);
//   2. the catalogue. The words the sheet prints are the words `ui.conf` stores, the send keys a
//      row may *offer* are exactly the ones `plan/key_gate.md` proves, and the font scale clamps
//      and quantizes to its 0.5..3.0 / 5 % grid;
//   3. the rows. Three sections plus `ALL`, a header per section in the list, the search filtering
//      label and section (not prose), the *effective* value shown for a malformed profile, and one
//      selectable row per id;
//   4. the keys. `Tab`/`Shift+Tab` walk the sections, `Ctrl+P`/`Ctrl+N` walk the rows, `Enter`
//      changes the selected value, a second click changes it and the first only selects it, and
//      the title row's `Close` control is the pointer's way out;
//   5. the frame wiring. The top bar's `Settings` entry opens it, `Esc` closes it as one step of
//      one press, the z-order is palette > settings > Raw Log, and the rectangle comes from
//      `ui::computeShell()` (inside the display, never part of the tiling);
//   6. persistence. A behaviour value round-trips through `ui.conf` (and the legacy `imgui2.conf`
//      round trip is `./test_settings`'s job, unchanged), a value the sheet can no longer offer is
//      still *shown*, and an unwritable profile produces exactly one Activity warning while the
//      session keeps running - never one per frame;
//   7. the copy action: the six documented fields, and no message text;
//   8. the goldens: open, with an active search, with no match, on a section, and at the 40x15
//      floor.
//
// Exit codes: 0 = all checks pass; 1 = a check failed; 2 = usage error.
//
//   ./test_settings_sheet [-v] [--update] [--print]
//
// `--update` rewrites the fixtures; do that only for a deliberate rendering change, and commit the
// diff with it.
//
// The GUI half of "a golden screen in both backends" is `./test_settings_gui`: the pixel sheet's
// type sizes, its search field's frame height and its placement only exist under the real
// GLFW/OpenGL3 backend (a text harness pins one cell to one cell).

#include "views/settings.h"

#include "views/shell.h"
#include "views/topbar.h"

#include "tui.h"

#include "model/activity.h"
#include "model/settings.h"
#include "model/ui_state.h"
#include "ui/layout.h"
#include "ui/metrics.h"
#include "ui/primitives.h"
#include "ui/text.h"

#include "imtui/imtui.h"

#include "imgui/imgui.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#include <sys/stat.h>
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
const char* g_phase = "start";

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

std::string trimTrailingSpaces(std::string text) {
    while (!text.empty() && text.back() == ' ')
        text.pop_back();
    return text;
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
    std::vector<unsigned char> bg;
    std::vector<unsigned char> fg;

    uint32_t at(int x, int y) const {
        if (x < 0 || y < 0 || x >= nx || y >= ny)
            return 0;
        return ch[static_cast<std::size_t>(y) * nx + x];
    }

    /// The background colour of one cell, as the backend's palette index: the only way a golden
    /// can pin a *fill* (the active section's inversion).
    int bgAt(int x, int y) const {
        if (x < 0 || y < 0 || x >= nx || y >= ny)
            return -1;
        return bg[static_cast<std::size_t>(y) * nx + x];
    }

    /// The foreground colour of one cell: what a *glyph* mark is drawn in (the selected row's
    /// rule cannot be a background on the grid - the `selection` fill quantizes onto the surface).
    int fgAt(int x, int y) const {
        if (x < 0 || y < 0 || x >= nx || y >= ny)
            return -1;
        return fg[static_cast<std::size_t>(y) * nx + x];
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
    grid.bg.resize(cells);
    grid.fg.resize(cells);
    for (std::size_t i = 0; i < cells; ++i) {
        grid.ch[i] = g_screen.data[i].ch;
        grid.bg[i] = g_screen.data[i].bg;
        grid.fg[i] = g_screen.data[i].fg;
    }
    return grid;
}

void dump(const Grid& grid) {
    std::fprintf(stderr, "grid (%dx%d):\n", grid.nx, grid.ny);
    for (int y = 0; y < grid.ny; ++y)
        std::fprintf(stderr, "%2d |%s|\n", y, trimTrailingSpaces(grid.rowText(y)).c_str());
}

void checkGrid(bool ok, const std::string& what) {
    check(ok, what);
    if (!ok)
        dump(capture());
}

/// One frame of the text grid at `g_w x g_h`: a decoration-free root window, the body drawing
/// inside it, then the frame through the real backend. `inject` runs *before* `NewFrame`, which is
/// when ImGui consumes its input queue.
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
    ImGui::Begin("settings-frame", nullptr,
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

/// The whole workbench frame (the shell plus the overlay layer) at `w x h`, exactly as the frame
/// layer composes it - one `TuiState`, no second renderer.
views::ShellFrame shellFrame(tui::TuiState& state, int w, int h,
                             const std::function<void(ImGuiIO&)>& inject = nullptr) {
    views::ShellFrame out;
    g_w = w;
    g_h = h;
    frame(
        [&](const ui::Metrics&) {
            out = views::renderShell(state, ImVec2(static_cast<float>(w), static_cast<float>(h)));
            views::renderOverlays(state, out);
        },
        inject);
    return out;
}

/// One frame through the real frame layer (`tuiRender`): the keymap, the `Esc` chain and the
/// `ui.conf` lifecycle, which the shell-only harness above does not run.
void appFrame(tui::TuiState& state, int w, int h,
              const std::function<void(ImGuiIO&)>& inject = nullptr) {
    g_w = w;
    g_h = h;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(w), static_cast<float>(h));
    io.DeltaTime = 1.0f / 60.0f;
    if (inject)
        inject(io);
    ImTui_ImplText_NewFrame();
    ImGui::NewFrame();
    if (!tui::tuiRender(state)) {
        std::fprintf(stderr, "FAIL [%s]: tuiRender reported 'user terminated'\n", g_phase);
        std::exit(1);
    }
    ImGui::Render();
    ImTui_ImplText_RenderDrawData(ImGui::GetDrawData(), &g_screen);
}

void pressKey(tui::TuiState& state, int w, int h, ImGuiKey key) {
    appFrame(state, w, h, [&](ImGuiIO& io) { io.AddKeyEvent(key, true); });
    appFrame(state, w, h, [&](ImGuiIO& io) { io.AddKeyEvent(key, false); });
}

void pressCtrl(tui::TuiState& state, int w, int h, ImGuiKey key) {
    appFrame(state, w, h, [&](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Ctrl, true); });
    appFrame(state, w, h, [&](ImGuiIO& io) {
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(key, true);
    });
    appFrame(state, w, h, [&](ImGuiIO& io) {
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(key, false);
    });
    appFrame(state, w, h, [&](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Ctrl, false); });
}

/// Move the keyboard from the search field into the list the way a user does - a cursor key - and
/// leave the selection where it was: `Ctrl+N` then `Ctrl+P` (the second press clamps back). Used by
/// the checks that press `Enter`, which belongs to the *list* only while the list owns the
/// keyboard.
void focusList(tui::TuiState& state, int w, int h) {
    pressCtrl(state, w, h, ImGuiKey_N);
    pressCtrl(state, w, h, ImGuiKey_P);
    shellFrame(state, w, h);
}

/// A click is a sequence: the pointer move, the press and the release each take their own frame
/// (ImGui trickles one event per frame), and the release must be delivered or the held button
/// latches `ActiveId` and blocks every later hover.
void clickAt(tui::TuiState& state, int w, int h, float x, float y) {
    shellFrame(state, w, h, [&](ImGuiIO& io) { io.AddMousePosEvent(x, y); });
    shellFrame(state, w, h);
    shellFrame(state, w, h, [&](ImGuiIO& io) {
        io.AddMousePosEvent(x, y);
        io.AddMouseButtonEvent(0, true);
    });
    shellFrame(state, w, h, [&](ImGuiIO& io) {
        io.AddMousePosEvent(x, y);
        io.AddMouseButtonEvent(0, false);
    });
    shellFrame(state, w, h);
    shellFrame(state, w, h);
}

/// A workbench state with a session and nothing else: the sheet's checks are about the sheet.
tui::TuiState makeState() {
    tui::TuiState state;
    state.readyStatus = true;
    tui::SessionEntry entry;
    entry.id = "s1";
    entry.title = "settings";
    entry.preview = "hello";
    entry.messageCount = 2;
    entry.isActive = true;
    state.sessionPanel.sessions.push_back(entry);
    state.sessionPanel.activeId = "s1";
    return state;
}

const views::SettingsRow* findRow(const std::vector<views::SettingsRow>& rows, const char* id) {
    for (const views::SettingsRow& row : rows) {
        if (std::strcmp(row.id, id) == 0)
            return &row;
    }
    return nullptr;
}

int rowIndex(const std::vector<views::SettingsRow>& rows, const char* id) {
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (std::strcmp(rows[i].id, id) == 0)
            return static_cast<int>(i);
    }
    return -1;
}

/// A scratch directory for the profile checks (the test's own, removed on the way in).
std::string scratchDir(const std::string& name) {
    const std::string root = "test_settings_sheet_scratch";
    ::mkdir(root.c_str(), 0755);
    const std::string dir = root + "/" + name;
    ::mkdir(dir.c_str(), 0755);
    return dir;
}

void removeFile(const std::string& path) { std::remove(path.c_str()); }

// ---------------------------------------------------------------------------
// 1. The source
// ---------------------------------------------------------------------------

void checkSource() {
    g_phase = "S: source";
    const std::string cpp = readWhole(std::string(TEST_SETTINGS_VIEW_SOURCE));
    const std::string header = readWhole(std::string(TEST_SETTINGS_HEADER_SOURCE));
    check(!cpp.empty(), "the sheet's source is readable");
    check(!header.empty(), "and so is its header");
    check(!contains(cpp, "#include \"tui.h\""), "views/settings.cpp includes no tui.h (D-ADAPTER)");
    check(!contains(cpp, "ChatMessage"), "and names no v4 message type");
    check(!contains(cpp, "RenderGroup"), "and no v4 render group");
    check(!contains(cpp, "displaySize()"), "and reads no display size (D-METRICS)");
    check(!contains(cpp, "DisplaySize"), "not even through ImGui's own name for it");
    check(!contains(cpp, "ImVec4("), "and writes no colour literal (D-THEME)");
    check(!contains(cpp, "tuiIsTextGrid"), "and asks no profile flag (the metrics answer it)");
    check(!contains(header, "#include \"tui.h\""), "views/settings.h includes no tui.h either");
    // The vocabulary lives in the header: the .cpp draws the strings, never a literal of its own.
    check(!contains(cpp, "\"Settings\""), "the title is the header's string, not a literal");
    check(!contains(cpp, "\"No setting matches\""), "and so is the empty state");
    check(!contains(cpp, "\"Copy system info\""), "and the copy row's label");
    // The sheet edits values; the *frame* owns both write paths (ui.conf's debounce, imgui2.conf's
    // save), so a view that opened a file would be a second writer nobody asked for.
    check(!contains(cpp, "saveTuiSettings("), "the view never writes imgui2.conf");
    check(!contains(cpp, "saveUiConf") && !contains(cpp, "flushUiConf"),
          "and never writes ui.conf");
    check(!contains(cpp, "fopen"), "and opens no file at all");
    // The `Enter` gate is the sheet's own state: `io.WantTextInput` is process-global and a frame
    // stale, so a composer field that was active when the sheet opened would make the first press
    // inert with nothing on screen to explain why (review round 2, N2).
    check(!contains(cpp, "WantTextInput"), "the Enter gate reads no process-global focus flag");
    // The model's catalogue is the one owner of the stored words.
    check(!contains(cpp, "\"enternewline\""), "the send-key words come from the model, not a copy");
}

// ---------------------------------------------------------------------------
// 2. The catalogue (model/settings.h)
// ---------------------------------------------------------------------------

void checkCatalogue() {
    g_phase = "C: catalogue";
    check(std::string(model::sendKeyWord(model::SendKey::Enter)) == "enter", "SendKey enter word");
    check(std::string(model::sendKeyWord(model::SendKey::CtrlEnter)) == "ctrlenter",
          "SendKey ctrlenter word");
    check(std::string(model::sendKeyWord(model::SendKey::EnterNewline)) == "enterNewline",
          "SendKey enterNewline word");

    // The key gate's answer: exactly one value may be offered (plan/key_gate.md §3).
    check(model::sendKeyProven(model::SendKey::EnterNewline), "enterNewline is the proven value");
    check(!model::sendKeyProven(model::SendKey::Enter),
          "enter is not proven (Shift+Enter is the same byte)");
    check(!model::sendKeyProven(model::SendKey::CtrlEnter), "ctrlenter is not proven (not bound)");
    check(model::kSendKeyChoiceCount == 3, "the table lists the design's three values");
    check(model::sendKeyChoice(0) == model::SendKey::Enter, "the table starts at `enter`");
    check(model::sendKeyChoice(1) == model::SendKey::CtrlEnter, "then `ctrlenter`");
    check(model::sendKeyChoice(2) == model::SendKey::EnterNewline, "then `enterNewline`");
    check(model::sendKeyChoice(-5) == model::SendKey::Enter, "an index below the table clamps");
    check(model::sendKeyChoice(99) == model::SendKey::EnterNewline, "and one above it clamps");
    check(model::sendKeyIndex(model::SendKey::CtrlEnter) == 1, "the index is the inverse");
    // The count of proven values is one: the sheet must never offer the other two, and this is the
    // number that says so rather than a comment.
    int proven = 0;
    for (int i = 0; i < model::kSendKeyChoiceCount; ++i) {
        if (model::sendKeyProven(model::sendKeyChoice(i)))
            ++proven;
    }
    check(proven == 1, "exactly one send key is proven");

    check(std::string(model::uiLogWord(model::kRawLogOff)) == "off", "UiLog off");
    check(std::string(model::uiLogWord(model::kRawLogRaw)) == "raw", "UiLog raw");
    check(std::string(model::uiLogWord(model::kRawLogDebug)) == "debug", "UiLog debug");
    check(std::string(model::uiLogWord(model::kRawLogTrace)) == "trace", "UiLog trace");
    check(std::string(model::uiLogWord(-3)) == "off", "a level below the range reads `off`");
    check(std::string(model::uiLogWord(99)) == "trace", "and one above it `trace`");

    // The font scale: clamped into 0.5..3.0 and quantized to 5 % steps.
    check(std::fabs(model::clampFontScale(0.2f) - 0.50f) < 1e-4f, "a scale below the range clamps");
    check(std::fabs(model::clampFontScale(9.0f) - 3.00f) < 1e-4f, "and above it clamps");
    check(std::fabs(model::clampFontScale(std::nanf("")) - 1.0f) < 1e-4f, "NaN is the default");
    check(std::fabs(model::clampFontScale(1.02f) - 1.00f) < 1e-4f, "a value between steps rounds");
    check(std::fabs(model::clampFontScale(1.03f) - 1.05f) < 1e-4f, "and a half step rounds up");
    check(std::fabs(model::stepFontScale(1.0f, 1) - 1.05f) < 1e-4f, "one step up");
    check(std::fabs(model::stepFontScale(1.0f, -1) - 0.95f) < 1e-4f, "one step down");
    check(std::fabs(model::stepFontScale(3.0f, 5) - 3.00f) < 1e-4f,
          "a step never leaves the range");
    check(std::fabs(model::stepFontScale(0.5f, -5) - 0.50f) < 1e-4f, "in either direction");
    check(model::fontScalePercent(1.25f) == 125, "the percentage the sheet prints");
    check(std::fabs(model::fontScaleFromPercent(250) - 2.5f) < 1e-4f, "and its inverse");
}

// ---------------------------------------------------------------------------
// 3. The rows
// ---------------------------------------------------------------------------

void checkRows() {
    g_phase = "R: rows";
    tui::TuiState state = makeState();
    model::ShellPrefs prefs = state.ui.prefs;
    tui::TuiSettings settings;
    const ui::Metrics& m = gridMetrics();

    views::SettingsState sheet;
    std::vector<views::SettingsRow> rows = views::settingsRows(sheet, prefs, settings, m);
    // The three sections' rows plus one header each: the `ALL` section shows everything.
    const int headers = 3;
    check(rows.size() == static_cast<std::size_t>(8 + headers),
          "ALL lists the eight settings under three section headers (got " +
              std::to_string(rows.size()) + ")");
    check(std::strcmp(rows.front().label.c_str(), views::kSettingsSectionAppearance) == 0,
          "the list starts with the APPEARANCE header");
    // Every id is unique and every row sits under its own section's header.
    bool idsUnique = true;
    bool sectionsCorrect = true;
    for (std::size_t i = 1; i < rows.size(); ++i) {
        if (rows[i].id[0] == '\0')
            continue;
        for (std::size_t j = i + 1; j < rows.size(); ++j) {
            if (rows[j].id[0] != '\0' && std::strcmp(rows[i].id, rows[j].id) == 0)
                idsUnique = false;
        }
        std::size_t header = i;
        while (header > 0 && rows[header].id[0] != '\0')
            --header;
        if (std::strcmp(rows[header].label.c_str(), rows[i].section) != 0)
            sectionsCorrect = false;
    }
    check(idsUnique, "every row id is unique");
    check(sectionsCorrect, "every row sits under its own section's header");

    // Each of the three sections on its own.
    for (int section = 1; section < views::kSettingsSectionCount; ++section) {
        sheet.section = section;
        const std::vector<views::SettingsRow> filtered =
            views::settingsRows(sheet, prefs, settings, m);
        check(!filtered.empty(),
              std::string("section ") + views::settingsSectionName(section) + " has rows");
        bool onlyOwn = true;
        for (const views::SettingsRow& row : filtered) {
            if (std::strcmp(row.section, views::settingsSectionName(section)) != 0)
                onlyOwn = false;
        }
        check(onlyOwn, std::string("section ") + views::settingsSectionName(section) +
                           " shows only its own rows");
    }
    sheet.section = 0;

    // The send-key row reports the gate's value and edits nothing.
    const views::SettingsRow* sendKey = findRow(rows, views::kSettingsRowSendKey);
    check(sendKey != nullptr, "the send-key row exists");
    if (sendKey != nullptr) {
        check(sendKey->kind == views::SettingsRowKind::ReadOnly, "it is not a choice");
        check(!sendKey->enabled, "and not activatable");
        check(sendKey->value == model::sendKeyWord(prefs.sendKey), "it prints the effective value");
        check(contains(sendKey->note, "Send"), "and says what sends instead");
    }

    // A hand-edited, unproven send key is *shown* (it is the effective value) but never offered.
    bool offeredNothingUnproven = true;
    for (int i = 0; i < model::kSendKeyChoiceCount; ++i) {
        prefs.sendKey = model::sendKeyChoice(i);
        const std::vector<views::SettingsRow> each = views::settingsRows(sheet, prefs, settings, m);
        const views::SettingsRow* row = findRow(each, views::kSettingsRowSendKey);
        check(row != nullptr && row->value == model::sendKeyWord(prefs.sendKey),
              std::string("the send-key row prints `") + model::sendKeyWord(prefs.sendKey) + "`");
        if (row == nullptr || row->enabled || row->kind == views::SettingsRowKind::Choice)
            offeredNothingUnproven = false;
    }
    check(offeredNothingUnproven,
          "no send key is offered as a choice - the gate proves one value (R-01)");
    // The loop above left the prefs at the shipped value; rebuild the list the rest reads.
    rows = views::settingsRows(sheet, prefs, settings, m);

    // The Raw log level is a choice over the four words, all of which the writer can spell.
    const views::SettingsRow* rawLog = findRow(rows, views::kSettingsRowRawLog);
    check(rawLog != nullptr && rawLog->kind == views::SettingsRowKind::Choice,
          "the Raw log level is a choice");
    check(rawLog != nullptr && rawLog->value == "off", "and starts at the shipped default");
    prefs.rawLog = 9; // out of range: the sheet shows the value the writer would store
    rows = views::settingsRows(sheet, prefs, settings, m);
    rawLog = findRow(rows, views::kSettingsRowRawLog);
    check(rawLog != nullptr && rawLog->value == "trace", "an out-of-range UiLog reads as `trace`");
    prefs.rawLog = model::kRawLogOff;

    // The font scale is a fact on the grid and a choice in the pixel profile.
    rows = views::settingsRows(sheet, prefs, settings, m);
    const views::SettingsRow* scale = findRow(rows, views::kSettingsRowFontScale);
    check(scale != nullptr, "the text-scale row exists");
    check(scale != nullptr && scale->kind == views::SettingsRowKind::ReadOnly,
          "on the text grid it is read-only (a cell is the terminal's size)");
    check(scale != nullptr && !scale->enabled, "and not activatable");
    settings.fontScaleMain = 1.25f;
    const std::vector<views::SettingsRow> guiRows =
        views::settingsRows(sheet, prefs, settings, ui::metricsFor(false, 1.0f, 1.0f));
    scale = findRow(guiRows, views::kSettingsRowFontScale);
    check(scale != nullptr && scale->kind == views::SettingsRowKind::Choice,
          "in the pixel profile it is a choice");
    check(scale != nullptr && scale->value == "125 %", "showing the scale as a percentage");
    settings.fontScaleMain = 1.0f;

    // The diagnostics rows report the frame's derived facts, not a guess.
    sheet.uiConfPath = "/tmp/ui.conf";
    sheet.iniConfPath = "/tmp/imgui2.conf";
    sheet.logArchive = true;
    rows = views::settingsRows(sheet, prefs, settings, m);
    const views::SettingsRow* uiConf = findRow(rows, views::kSettingsRowUiConf);
    check(uiConf != nullptr && uiConf->value == "/tmp/ui.conf", "the ui.conf path is drawn");
    const views::SettingsRow* iniConf = findRow(rows, views::kSettingsRowIniConf);
    check(iniConf != nullptr && iniConf->value == "/tmp/imgui2.conf",
          "and so is the imgui2.conf path");
    const views::SettingsRow* logLevel = findRow(rows, views::kSettingsRowLogLevel);
    check(logLevel != nullptr && logLevel->value == views::kSettingsLogLevelOn,
          "an archive that is on reads `all`");
    sheet.logArchive = false;
    rows = views::settingsRows(sheet, prefs, settings, m);
    logLevel = findRow(rows, views::kSettingsRowLogLevel);
    check(logLevel != nullptr && logLevel->value == views::kSettingsLogLevelOff,
          "and one that is off reads `off`");
    sheet.uiConfPath.clear();
    rows = views::settingsRows(sheet, prefs, settings, m);
    uiConf = findRow(rows, views::kSettingsRowUiConf);
    check(uiConf != nullptr && uiConf->value == views::kSettingsNoProfile,
          "no profile name is said, not left blank");

    // The search: over the label and the section, never the prose.
    sheet.search.fill(0);
    std::snprintf(sheet.search.data(), sheet.search.size(), "%s", "log");
    rows = views::settingsRows(sheet, prefs, settings, m);
    check(findRow(rows, views::kSettingsRowRawLog) != nullptr, "`log` finds the Raw log level");
    check(findRow(rows, views::kSettingsRowLogLevel) != nullptr, "and the log level");
    check(findRow(rows, views::kSettingsRowFontScale) == nullptr, "and nothing else");
    check(findRow(rows, views::kSettingsRowBackend) == nullptr, "not the backend row");
    std::snprintf(sheet.search.data(), sheet.search.size(), "%s", "ADVANCED");
    rows = views::settingsRows(sheet, prefs, settings, m);
    check(rows.size() == 6, "the section name matches all five ADVANCED rows (+ its header)");
    std::snprintf(sheet.search.data(), sheet.search.size(), "%s", "RESTART");
    rows = views::settingsRows(sheet, prefs, settings, m);
    check(rows.empty(), "a search that only matches a note matches nothing");
    std::snprintf(sheet.search.data(), sheet.search.size(), "%s", "   ");
    rows = views::settingsRows(sheet, prefs, settings, m);
    check(rows.size() == 11, "a blanks-only search is no search");
    sheet.search.fill(0);
}

// ---------------------------------------------------------------------------
// 4. The keys
// ---------------------------------------------------------------------------

void checkKeys() {
    g_phase = "K: keys";
    tui::TuiState state = makeState();
    const int w = 100;
    const int h = 30;
    appFrame(state, w, h);
    views::openSettings(state.settingsSheet);
    appFrame(state, w, h);
    appFrame(state, w, h);
    check(state.settingsSheet.open, "the sheet is open");
    check(state.settingsSheet.rowsBuilt == 11, "and built its rows");

    // The field takes the keyboard on open (the `F` request's one-shot answer), and what is typed
    // into it filters the rows live - the field owns the characters while it holds the keyboard.
    check(state.settingsSheet.searchFocused, "the search field owns the keyboard on open");
    for (const char c : std::string("log"))
        appFrame(state, w, h, [&](ImGuiIO& io) { io.AddInputCharacter(c); });
    check(state.settingsSheet.searchFocused, "the field keeps the keyboard while typing");
    check(std::string(state.settingsSheet.search.data()) == "log", "the query is in the field");
    check(state.settingsSheet.rowsBuilt == 4,
          "`log` leaves the two matching rows under their two headers (got " +
              std::to_string(state.settingsSheet.rowsBuilt) + ")");
    checkGrid(capture().hasText("Raw log level"), "and the row is on screen");

    // Review finding: `Enter` belongs to the field while the field has the keyboard. A press that
    // means "done typing" must never cycle a persisted preference (which `ui.conf` would then
    // store). The filtered list leaves the cursor on the Raw log level, so this is a real target.
    check(state.settingsSheet.selected == 1, "the filter leaves the cursor on the Raw log row");
    const int beforeFieldEnter = state.ui.prefs.rawLog;
    pressKey(state, w, h, ImGuiKey_Enter);
    check(state.ui.prefs.rawLog == beforeFieldEnter,
          "Enter in the search field edits nothing (the field owns it)");
    check(state.settingsSheet.open, "and does not close the sheet");
    // With the keyboard in the list the same press does edit the row - and a cursor key is what
    // moves it there (the same key that moved the cursor answered the question the field was for).
    focusList(state, w, h);
    check(!state.settingsSheet.searchFocused, "a cursor key takes the keyboard off the field");
    check(std::string(state.settingsSheet.search.data()) == "log",
          "and the query the field held survives the hand-over (the buffer is the caller's)");
    pressKey(state, w, h, ImGuiKey_Enter);
    check(state.ui.prefs.rawLog == (beforeFieldEnter + 1) % model::kUiLogChoiceCount,
          "and Enter then changes the selected row (got " + std::to_string(state.ui.prefs.rawLog) +
              ")");
    state.ui.prefs.rawLog = beforeFieldEnter;

    // Back to the whole list: the buffer is rewritten under a fresh widget id, so the field
    // re-reads it instead of its stale edit state (the rail's `filterSeq` rule).
    state.settingsSheet.search.fill('\0');
    ++state.settingsSheet.searchSeq;
    appFrame(state, w, h);
    check(state.settingsSheet.rowsBuilt == 11, "clearing the field restores every row");

    // Tab walks the sections, Shift+Tab walks them back, and the cycle wraps.
    check(state.settingsSheet.section == 0, "the strip starts on ALL");
    pressKey(state, w, h, ImGuiKey_Tab);
    check(state.settingsSheet.section == 1, "Tab selects APPEARANCE");
    pressKey(state, w, h, ImGuiKey_Tab);
    check(state.settingsSheet.section == 2, "Tab again selects BEHAVIOUR");
    appFrame(state, w, h, [](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Shift, true); });
    appFrame(state, w, h, [](ImGuiIO& io) {
        io.AddKeyEvent(ImGuiMod_Shift, true);
        io.AddKeyEvent(ImGuiKey_Tab, true);
    });
    appFrame(state, w, h, [](ImGuiIO& io) {
        io.AddKeyEvent(ImGuiMod_Shift, true);
        io.AddKeyEvent(ImGuiKey_Tab, false);
    });
    appFrame(state, w, h, [](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Shift, false); });
    check(state.settingsSheet.section == 1, "Shift+Tab walks back to APPEARANCE");
    pressKey(state, w, h, ImGuiKey_Tab);
    check(state.settingsSheet.section == 2, "and forward again");
    pressKey(state, w, h, ImGuiKey_Tab);
    check(state.settingsSheet.section == 3, "to the last section");
    pressKey(state, w, h, ImGuiKey_Tab);
    check(state.settingsSheet.section == 0, "and the cycle wraps round to ALL");

    // Back to ALL, then walk the rows with Ctrl+N / Ctrl+P. The cursor steps *over* a section
    // header: row 1 is the first setting, row 2 the BEHAVIOUR header, so one press reaches row 3.
    state.settingsSheet.section = 0;
    appFrame(state, w, h);
    check(state.settingsSheet.selected == 1, "the cursor starts on the first setting");
    pressCtrl(state, w, h, ImGuiKey_N);
    check(state.settingsSheet.selected == 3, "Ctrl+N steps over the section header (got " +
                                                 std::to_string(state.settingsSheet.selected) +
                                                 ")");
    pressCtrl(state, w, h, ImGuiKey_N);
    check(state.settingsSheet.selected == 4, "and reaches the next setting");
    pressCtrl(state, w, h, ImGuiKey_P);
    check(state.settingsSheet.selected == 3, "Ctrl+P walks back");
    pressCtrl(state, w, h, ImGuiKey_P);
    check(state.settingsSheet.selected == 1, "over the header again, to the first setting");
    pressCtrl(state, w, h, ImGuiKey_P);
    check(state.settingsSheet.selected == 1, "and stops there (no wrap)");

    // The Raw log level: select it and press Enter four times - the four values in order.
    const int rawLogRow = rowIndex(state.settingsSheet.rows, views::kSettingsRowRawLog);
    check(rawLogRow > 0, "the Raw log row is in the list");
    state.settingsSheet.selected = rawLogRow;
    appFrame(state, w, h);
    const int before = state.ui.prefs.rawLog;
    pressKey(state, w, h, ImGuiKey_Enter);
    check(state.ui.prefs.rawLog == (before + 1) % model::kUiLogChoiceCount,
          "Enter moves the Raw log level to the next value (got " +
              std::to_string(state.ui.prefs.rawLog) + ")");
    pressKey(state, w, h, ImGuiKey_Enter);
    pressKey(state, w, h, ImGuiKey_Enter);
    pressKey(state, w, h, ImGuiKey_Enter);
    check(state.ui.prefs.rawLog == before, "four presses come back round to the start");

    // The read-only rows do nothing: the send key is the gate's one value, not a choice.
    const int sendKeyRow = rowIndex(state.settingsSheet.rows, views::kSettingsRowSendKey);
    state.settingsSheet.selected = sendKeyRow;
    appFrame(state, w, h);
    pressKey(state, w, h, ImGuiKey_Enter);
    check(state.ui.prefs.sendKey == model::SendKey::EnterNewline,
          "Enter on the send-key row changes nothing (the gate offers one value)");
    const int uiConfRow = rowIndex(state.settingsSheet.rows, views::kSettingsRowUiConf);
    state.settingsSheet.selected = uiConfRow;
    appFrame(state, w, h);
    pressKey(state, w, h, ImGuiKey_Enter);
    check(state.settingsSheet.open,
          "Enter on a diagnostics row only does nothing (the sheet stays)");

    // Review round 2 (N1): the *first* `Enter` after a `Tab` must change the row - from the state a
    // user is actually in when they press Tab, with the search field holding the keyboard. ImGui's
    // Tab-nav re-activates the window's only text field on the Tab-*release* frame, so a one-shot
    // blur is undone a frame later and the press is spent leaving a field nobody asked for.
    {
        tui::TuiState tabbed = makeState();
        appFrame(tabbed, w, h);
        views::openSettings(tabbed.settingsSheet);
        appFrame(tabbed, w, h);
        appFrame(tabbed, w, h);
        check(tabbed.settingsSheet.searchFocused, "a fresh sheet's field holds the keyboard");
        for (const char c : std::string("raw log"))
            appFrame(tabbed, w, h, [&](ImGuiIO& io) { io.AddInputCharacter(c); });
        check(std::string(tabbed.settingsSheet.search.data()) == "raw log",
              "the query is in the field");
        check(tabbed.settingsSheet.rowsBuilt == 2, "and narrows the list to the Raw log row");
        // The query hides the send-key row, so BEHAVIOUR's first setting *is* the Raw log level:
        // the cursor lands on an editable row without a cursor key having moved the keyboard into
        // the list (a cursor key is the path the earlier checks already prove).
        pressKey(tabbed, w, h, ImGuiKey_Tab);
        pressKey(tabbed, w, h, ImGuiKey_Tab);
        check(tabbed.settingsSheet.section == 2, "two Tabs reach BEHAVIOUR");
        check(!tabbed.settingsSheet.searchFocused,
              "and the field stays out of the keyboard (the list owns it)");
        const int tabRawLogRow = rowIndex(tabbed.settingsSheet.rows, views::kSettingsRowRawLog);
        check(tabRawLogRow > 0 && tabbed.settingsSheet.selected == tabRawLogRow,
              "the cursor sits on the Raw log row");
        // The latch's trade-off, pinned rather than left to the comment: with the list owning the
        // keyboard a typed character neither reaches the search nor filters the list - `/` (or a
        // click) is what gives the field back.
        const std::size_t rowsAfterTab = tabbed.settingsSheet.rowsBuilt;
        appFrame(tabbed, w, h, [](ImGuiIO& io) { io.AddInputCharacter('z'); });
        check(std::string(tabbed.settingsSheet.search.data()) == "raw log",
              "a character typed after a Tab does not reach the search");
        check(tabbed.settingsSheet.rowsBuilt == rowsAfterTab, "and filters nothing");
        const int beforeTabEnter = tabbed.ui.prefs.rawLog;
        pressKey(tabbed, w, h, ImGuiKey_Enter);
        check(tabbed.ui.prefs.rawLog == (beforeTabEnter + 1) % model::kUiLogChoiceCount,
              "the first Enter after a Tab changes the row (got " +
                  std::to_string(tabbed.ui.prefs.rawLog) + ")");
    }

    // `/` is the advertised way back to the search, and it must work on the frame right after a
    // `Tab`: `ui::takeCharacter()`'s text-field guard is process-global and still set by the
    // field's own stale re-activation for a couple of frames, so the sheet takes the character
    // without the guard while its field is released (review M7 - before that an early `/` was
    // neither typed nor handled, so the only way back lost its first press).
    {
        tui::TuiState slash = makeState();
        appFrame(slash, w, h);
        views::openSettings(slash.settingsSheet);
        appFrame(slash, w, h);
        appFrame(slash, w, h);
        pressKey(slash, w, h, ImGuiKey_Tab);
        check(!slash.settingsSheet.searchFocused, "a Tab releases the field");
        appFrame(slash, w, h, [](ImGuiIO& io) { io.AddInputCharacter('/'); });
        check(slash.settingsSheet.searchFocus || slash.settingsSheet.searchFocused,
              "`/` on the very next frame reaches the field again");
        appFrame(slash, w, h);
        check(slash.settingsSheet.searchFocused, "the field then takes the keyboard");
        appFrame(slash, w, h, [](ImGuiIO& io) { io.AddInputCharacter('q'); });
        check(std::string(slash.settingsSheet.search.data()) == "q", "and typing continues there");
        // Leave the shared ImGui context with no active field: every harness state lives in one
        // context and the sheet's window/item ids are the same for all of them, so a field left
        // active here would be inherited (with its text) by the next state's sheet.
        pressKey(slash, w, h, ImGuiKey_Tab);
        shellFrame(slash, w, h);
    }
}

/// A click selects; a second click on the selected row changes it.
void checkClicks() {
    g_phase = "K: clicks";
    tui::TuiState state = makeState();
    const int w = 100;
    const int h = 30;
    appFrame(state, w, h);
    views::openSettings(state.settingsSheet);
    const views::ShellFrame out = shellFrame(state, w, h);
    const ui::Rect sheet = out.layout.settingsOverlay;
    check(sheet.w > 0.0f && sheet.h > 0.0f, "the sheet has a rectangle");
    const int rawLogRow = rowIndex(state.settingsSheet.rows, views::kSettingsRowRawLog);
    check(rawLogRow > 0, "the Raw log row is in the list");
    // The grid's anatomy: one row for the frame, then title, search and strip, then the body - so
    // body row `n` is `sheet.y + 4 + n`. A click lands in the middle of its row.
    const float x = sheet.x + 6.0f;
    const float y = sheet.y + 4.0f + static_cast<float>(rawLogRow) + 0.5f;

    // Click 1: select. Click 2: change.
    const int before = state.ui.prefs.rawLog;
    clickAt(state, w, h, x, y);
    check(state.settingsSheet.selected == rawLogRow,
          "a click selects the row under the pointer (got " +
              std::to_string(state.settingsSheet.selected) + ")");
    check(state.ui.prefs.rawLog == before, "but a first click never edits a preference");
    clickAt(state, w, h, x, y);
    check(state.ui.prefs.rawLog == (before + 1) % model::kUiLogChoiceCount,
          "a second click on the same row changes it");

    // The strip: clicking a section selects it.
    clickAt(state, w, h, sheet.x + 1.0f + 12.0f, sheet.y + 3.5f);
    check(state.settingsSheet.section == 1,
          "a click on the strip's APPEARANCE entry selects it (got " +
              std::to_string(state.settingsSheet.section) + ")");

    // The `Close` control in the title row: the pointer's way out (a modal that only `Esc` closes
    // is the mirror image of an action hidden behind the mouse).
    const ui::ButtonSpec closeSpec{views::kSettingsCloseLabel};
    const float closeW = ui::measureButton(gridMetrics(), closeSpec).w;
    check(closeW > 0.0f, "the title row measures the Close control");
    clickAt(state, w, h, sheet.right() - 1.0f - closeW * 0.5f, sheet.y + 1.5f);
    check(!state.settingsSheet.open, "clicking Close closes the sheet");
    check(state.settingsSheet.search[0] == '\0', "and leaves no query behind");
}

// ---------------------------------------------------------------------------
// 5. The frame wiring
// ---------------------------------------------------------------------------

void checkFrameWiring() {
    g_phase = "F: wiring";
    const int w = 120;
    const int h = 36;
    tui::TuiState state = makeState();

    // Closed to begin with: no rectangle, and nothing for `Esc` to close.
    views::ShellFrame out = shellFrame(state, w, h);
    check(out.layout.settingsOverlay.w == 0.0f, "a closed sheet has no rectangle");
    check(!views::escStepClosesOverlay(state), "and `Esc` has nothing of its own to close");

    // The top bar's `Settings` entry is the visible control that opens it: click its box.
    const ui::Metrics& m = gridMetrics();
    const views::TopBarParts parts = views::topBarMeasure(state.topBar, out.layout.topBar, m);
    check(!parts.gear.empty(), "the bar reserved a Settings entry");
    const float gearX = parts.gear.x + parts.gear.w * 0.5f;
    const float gearY = parts.gear.y + parts.gear.h * 0.5f;
    clickAt(state, w, h, gearX, gearY);
    check(state.settingsSheet.open, "clicking Settings opens the sheet");
    const views::ShellFrame open = shellFrame(state, w, h);
    check(open.layout.settingsOverlay.w > 0.0f, "the open sheet has a rectangle");
    check(open.layout.settingsOverlay.x >= 0.0f &&
              open.layout.settingsOverlay.right() <= static_cast<float>(w) &&
              open.layout.settingsOverlay.y >= 0.0f &&
              open.layout.settingsOverlay.bottom() <= static_cast<float>(h),
          "and it is inside the display");
    check(open.layout.top == ui::OverlayLayer::Settings, "the sheet is the topmost overlay");
    check(state.overlayOwnsKeyboard(), "and it owns the keyboard (D-OVERLAY)");
    check(views::escStepClosesOverlay(state), "`Esc` closes it");

    // It does not re-tile: every region keeps the rectangle it had while the sheet was closed
    // (the palette's rule - an overlay is not part of the tiling).
    const bool sameTiling = open.layout.topBar.w == out.layout.topBar.w &&
                            open.layout.thread.w == out.layout.thread.w &&
                            open.layout.thread.h == out.layout.thread.h &&
                            open.layout.composer.h == out.layout.composer.h &&
                            open.layout.nowBar.w == out.layout.nowBar.w;
    check(sameTiling, "opening the sheet leaves the tiling alone");

    // The z-order: the palette above the sheet, the Raw Log below it.
    ui::LayoutPrefs prefs = views::layoutPrefsFor(state.ui, /*paletteOpen=*/true,
                                                  /*composerH=*/0.0f, /*settingsOpen=*/true);
    ui::ShellLayout both =
        views::shellLayoutFor(prefs, ImVec2(static_cast<float>(w), static_cast<float>(h)));
    check(both.top == ui::OverlayLayer::Palette, "the palette is above the sheet");
    check(both.settingsOverlay.w > 0.0f && both.paletteOverlay.w > 0.0f,
          "and both have rectangles while both are up");
    ui::LayoutPrefs rawPrefs;
    rawPrefs.rawLogOpen = true;
    rawPrefs.settingsOpen = true;
    const ui::ShellLayout withRaw =
        views::shellLayoutFor(rawPrefs, ImVec2(static_cast<float>(w), static_cast<float>(h)));
    check(withRaw.top == ui::OverlayLayer::Settings, "the sheet is above the Raw Log");

    // `Esc` closes exactly one surface per press, topmost first.
    tui::TuiState press = makeState();
    appFrame(press, w, h);
    views::openSettings(press.settingsSheet);
    appFrame(press, w, h);
    pressKey(press, w, h, ImGuiKey_Escape);
    check(!press.settingsSheet.open, "Esc closes the sheet");
    check(!press.palette.open, "and opens nothing else");

    // The sheet closed from the `Esc` step hands the keyboard back to the composer.
    check(press.composer.focusRequested || press.userQuery.isSubmitted,
          "closing the sheet hands the keyboard back to the composer");

    // With the palette also up, `Esc` closes the palette first (the sheet must survive one press).
    tui::TuiState stacked = makeState();
    appFrame(stacked, w, h);
    views::openSettings(stacked.settingsSheet);
    views::openPaletteForState(stacked.palette, stacked, "");
    appFrame(stacked, w, h);
    check(views::escStepClosesOverlay(stacked), "an overlay is up");
    // Isolate the close's effect: a fresh composer state *starts* with its own focus request, so
    // the two latches are cleared here and must stay clear (review finding: closing the palette
    // used to hand the composer a focus request *behind* the still-open sheet).
    stacked.composer.focusRequested = false;
    stacked.userQuery.isSubmitted = false;
    stacked.settingsSheet.searchFocus = false;
    stacked.settingsSheet.searchFocused = false;
    const ui::OverlayLayer closed = views::closeTopOverlay(stacked);
    check(closed == ui::OverlayLayer::Palette, "the first press closes the palette");
    check(stacked.settingsSheet.open, "and the sheet is still up");
    check(stacked.settingsSheet.searchFocus || stacked.settingsSheet.searchFocused,
          "the sheet takes the keyboard back (its search field)");
    // ... and the request really reaches the field: with the palette down, a character typed now is
    // the search's - the release latch a `Tab` sets must not outlive the hand-back (the interchange
    // review round 3 asked to see pinned).
    for (int i = 0; i < 2; ++i)
        appFrame(stacked, w, h);
    check(stacked.settingsSheet.searchFocused,
          "the field holds the keyboard again after the palette closes");
    appFrame(stacked, w, h, [](ImGuiIO& io) { io.AddInputCharacter('k'); });
    check(std::string(stacked.settingsSheet.search.data()) == "k",
          "and a character typed then lands in the search");
    check(!stacked.composer.focusRequested && !stacked.userQuery.isSubmitted,
          "and no focus request reaches the composer behind it");
    check(views::closeTopOverlay(stacked) == ui::OverlayLayer::Settings,
          "the second press closes the sheet");
    check(!stacked.settingsSheet.open, "which is now down");
    check(views::closeTopOverlay(stacked) == ui::OverlayLayer::None,
          "and a third press finds nothing to close");
    check(stacked.composer.focusRequested || stacked.userQuery.isSubmitted,
          "closing the last surface hands the keyboard back to the composer");

    // The `Close` answer (the sheet's own control) closes it too, and clears the search.
    tui::TuiState own = makeState();
    views::openSettings(own.settingsSheet);
    std::snprintf(own.settingsSheet.search.data(), own.settingsSheet.search.size(), "%s", "abc");
    own.settingsSheet.closeRequested = true;
    shellFrame(own, w, h);
    check(!own.settingsSheet.open, "the sheet's own close answer closes it");
    check(own.settingsSheet.search[0] == '\0', "and the next open starts from a clean search");
}

// ---------------------------------------------------------------------------
// 6. Persistence, malformed values and the read-only profile
// ---------------------------------------------------------------------------

void checkPersistence() {
    g_phase = "P: persistence";
    const std::string dir = scratchDir("conf");
    const std::string ini = dir + "/imgui2.ini";
    removeFile(model::uiConfPath(ini));

    // A hand-edited profile with a value the sheet may not offer, an out-of-range level and a
    // malformed word: the loader's rules are `./test_ui_settings`'s, and what matters here is that
    // the *sheet* shows the effective value of each.
    writeFile(model::uiConfPath(ini),
              "Version=1\nSendKey=ctrlenter\nUiLog=debug\nRailOpen=zoinks\nInspectorTab=agents\n");
    {
        tui::TuiState state = makeState();
        state.iniFilename = ini;
        appFrame(state, 100, 30); // the first workbench frame loads ui.conf
        check(state.ui.prefs.sendKey == model::SendKey::CtrlEnter,
              "a legal SendKey line reaches the preferences");
        check(state.ui.prefs.rawLog == model::kRawLogDebug, "and so does UiLog");
        check(state.ui.prefs.railOpen != 0, "a malformed RailOpen keeps the current value");
        check(state.ui.prefs.tab == model::InspectorTab::Agents, "and InspectorTab loads");

        const std::vector<views::SettingsRow> rows =
            views::settingsRows(state.settingsSheet, state.ui.prefs, state.settings, gridMetrics());
        const views::SettingsRow* sendKey = findRow(rows, views::kSettingsRowSendKey);
        check(sendKey != nullptr && sendKey->value == "ctrlenter",
              "the sheet shows the hand-edited send key, not a silent default");
        const views::SettingsRow* rawLog = findRow(rows, views::kSettingsRowRawLog);
        check(rawLog != nullptr && rawLog->value == "debug", "and the level it is running at");
    }

    // A change made in the sheet reaches ui.conf, and the level survives a restart.
    removeFile(model::uiConfPath(ini));
    {
        tui::TuiState state = makeState();
        state.iniFilename = ini;
        appFrame(state, 100, 30);
        views::openSettings(state.settingsSheet);
        appFrame(state, 100, 30);
        state.settingsSheet.selected =
            rowIndex(state.settingsSheet.rows, views::kSettingsRowRawLog);
        appFrame(state, 100, 30);
        focusList(state, 100, 30); // the keyboard has to be in the list before `Enter` edits a row
        pressKey(state, 100, 30, ImGuiKey_Enter);
        pressKey(state, 100, 30, ImGuiKey_Enter); // off -> raw -> debug
        check(state.ui.prefs.rawLog == model::kRawLogDebug, "two presses reach `debug`");
        // The debounced write needs the idle window; the quit path does not.
        views::uiConfQuit(state.ui, ini);
        const std::string text = readWhole(model::uiConfPath(ini));
        check(contains(text, "UiLog=debug"), "the level is on disk after a quit save");
        check(contains(text, "SendKey=enterNewline"), "with the send key the build offers");
    }
    {
        tui::TuiState restarted = makeState();
        restarted.iniFilename = ini;
        appFrame(restarted, 100, 30);
        check(restarted.ui.prefs.rawLog == model::kRawLogDebug,
              "and a restarted session reads it back");
    }

    // Every value the *file* can carry round-trips through the words the sheet prints - the send
    // keys are not offered by the sheet (the gate proves one), but a hand-edited profile still has
    // to survive a load and a save unchanged.
    {
        const std::string roundDir = scratchDir("round");
        const std::string roundIni = roundDir + "/imgui2.ini";
        for (int i = 0; i < model::kSendKeyChoiceCount; ++i) {
            const model::SendKey key = model::sendKeyChoice(i);
            removeFile(model::uiConfPath(roundIni));
            writeFile(model::uiConfPath(roundIni),
                      "Version=1\nSendKey=" + std::string(model::sendKeyWord(key)) +
                          "\nUiLog=trace\n");
            model::ShellPrefs loaded;
            model::loadUiConf(loaded, roundIni);
            check(loaded.sendKey == key,
                  std::string("`SendKey=") + model::sendKeyWord(key) + "` loads back");
            check(loaded.rawLog == model::kRawLogTrace, "and the level with it");
            check(contains(model::uiConfText(loaded),
                           std::string("SendKey=") + model::sendKeyWord(key)),
                  "and is written back with the same word");
        }
        for (int level = 0; level < model::kUiLogChoiceCount; ++level) {
            model::ShellPrefs prefs;
            prefs.rawLog = level;
            const std::string text = model::uiConfText(prefs);
            check(contains(text, std::string("UiLog=") + model::uiLogWord(level)),
                  std::string("`UiLog=") + model::uiLogWord(level) + "` is the stored word");
        }
    }

    // The font scale keeps its own file: the sheet's answer is one `saveTuiSettings` call, and the
    // value itself is `TuiSettings` (the struct the settings file has always used, unchanged).
    {
        const std::string scaleDir = scratchDir("scale");
        const std::string scaleIni = scaleDir + "/imgui2.ini";
        removeFile(tui::settingsPath(scaleIni));
        tui::TuiSettings settings;
        settings.fontScaleMain = model::stepFontScale(1.0f, 2);
        tui::saveTuiSettings(settings, scaleIni);
        tui::TuiSettings loaded;
        tui::loadTuiSettings(loaded, scaleIni);
        check(std::fabs(loaded.fontScaleMain - 1.10f) < 0.005f,
              "the font scale round-trips through imgui2.conf (the legacy file)");
    }

    // An unwritable profile: one Activity warning, the session keeps running, no per-frame retry.
    //
    // The clock is the frame layer's (`uiConfFrame`'s `nowSeconds`), so the check drives that call
    // with a hand-made clock instead of waiting two real seconds: the property under test is "one
    // attempt, one warning", not the wall clock.
    {
        const std::string blockedDir = scratchDir("blocked");
        writeFile(blockedDir + "/blocked", "not a directory\n");
        const std::string blockedIni = blockedDir + "/blocked/imgui2.ini";
        tui::TuiState state = makeState();
        state.iniFilename = blockedIni;
        bool loaded = false;
        views::uiConfFrame(state.ui, loaded, blockedIni, 0.0); // reads: there is no file
        check(loaded, "the profile was read (and found missing)");
        state.ui.prefs.rawLog = model::kRawLogDebug; // a change the user made in the sheet
        views::uiConfFrame(state.ui, loaded, blockedIni, 0.1); // arms the idle window
        check(state.ui.activity.empty(), "a change alone writes nothing yet");
        views::uiConfFrame(state.ui, loaded, blockedIni, 10.0); // the one attempt, which fails
        check(state.ui.activity.size() == 1, "the failed write is reported once");
        if (!state.ui.activity.empty()) {
            check(state.ui.activity.back().level == model::kActivityLevelWarn,
                  "as a warning (Activity's three-step scale)");
            check(contains(state.ui.activity.back().message, "cannot write"),
                  "and says what could not be written");
            check(contains(state.ui.activity.back().message, "ui.conf"), "naming the file");
        }
        for (int i = 0; i < 10; ++i)
            views::uiConfFrame(state.ui, loaded, blockedIni, 20.0 + i);
        check(state.ui.activity.size() == 1,
              "and never again for the same bytes (no per-frame retry)");
        check(state.ui.prefs.rawLog == model::kRawLogDebug,
              "while the value still applies in memory");
        // A *new* choice is a new attempt, and is reported once more - the user changed something.
        state.ui.prefs.rawLog = model::kRawLogTrace;
        views::uiConfFrame(state.ui, loaded, blockedIni, 100.0); // arms
        views::uiConfFrame(state.ui, loaded, blockedIni, 110.0); // attempt
        check(state.ui.activity.size() == 2, "a later change is its own single warning");
    }
}

// ---------------------------------------------------------------------------
// 7. Copy system info
// ---------------------------------------------------------------------------

void checkCopy() {
    g_phase = "Y: copy";
    views::SettingsState sheet;
    sheet.uiConfPath = "/home/u/.config/llmfun/ui.conf";
    sheet.iniConfPath = "/home/u/.config/llmfun/imgui2.conf";
    sheet.versions = "tui api 5, ui.conf v1";
    sheet.flags = "offline=0 run=1 stop=0 rawlog=0";
    sheet.displayW = 100;
    sheet.displayH = 30;

    const std::string text = views::systemInfoText(sheet, gridMetrics());
    check(contains(text, "backend: text grid (terminal)"), "the copy names the backend");
    check(contains(text, "display: 100x30 cells"), "the display it is drawn in");
    check(contains(text, "profile: text grid"), "the profile");
    check(contains(text, "generation: workbench"), "the generation");
    check(contains(text, "flags: offline=0 run=1 stop=0 rawlog=0"), "the run's flags");
    check(contains(text, "versions: tui api 5, ui.conf v1"), "and the versions");
    const std::string pixels = views::systemInfoText(sheet, ui::metricsFor(false, 1.0f, 1.0f));
    check(contains(pixels, "display: 100x30 px"), "a pixel display is labelled in pixels");
    check(contains(pixels, "profile: pixel"), "and names the pixel profile");
    // No message text, no session title, no transcript: the payload is the six fields.
    const char* forbidden[] = {"hello", "settings", "title", "query", "user:", "assistant:"};
    for (const char* word : forbidden)
        check(!contains(text, word), std::string("the copy carries no `") + word + "`");

    // The row itself: selecting it and pressing Enter is the action.
    tui::TuiState state = makeState();
    views::openSettings(state.settingsSheet);
    appFrame(state, 100, 30);
    focusList(state, 100, 30);
    state.settingsSheet.selected = rowIndex(state.settingsSheet.rows, views::kSettingsRowCopyInfo);
    appFrame(state, 100, 30);
    pressKey(state, 100, 30, ImGuiKey_Enter);
    check(state.settingsSheet.copiedUntil > 0.0, "Enter on the copy row answers");
    const Grid grid = capture();
    checkGrid(grid.hasText(views::kSettingsCopied), "and the sheet says so");
}

// ---------------------------------------------------------------------------
// 8. The goldens
// ---------------------------------------------------------------------------

struct Scene {
    const char* name;
    int section;
    const char* search;
    int w, h;
};

std::vector<Scene> scenes() {
    return {
        {"settings-open", 0, "", 100, 30},      {"settings-filtered", 0, "log", 100, 30},
        {"settings-empty", 0, "zzzz", 100, 30}, {"settings-section", 2, "", 100, 30},
        {"settings-floor", 0, "", 40, 15},
    };
}

std::string goldenText(const Scene& scene) {
    tui::TuiState state = makeState();
    g_w = scene.w;
    g_h = scene.h;
    // The scene's own state, set directly: `openSettings()` would also ask for the search field's
    // keyboard, and a caret that blinks with the frame clock has no place in a fixture.
    state.settingsSheet.open = true;
    state.settingsSheet.section = scene.section;
    if (scene.search != nullptr)
        std::snprintf(state.settingsSheet.search.data(), state.settingsSheet.search.size(), "%s",
                      scene.search);
    // Two frames, and the second one is captured: every harness shares one ImGui context, so a
    // single frame would inherit whatever active id the checks above left behind.
    shellFrame(state, scene.w, scene.h);
    const views::ShellFrame out = shellFrame(state, scene.w, scene.h);
    const Grid grid = capture();
    const ui::Rect box = out.layout.settingsOverlay;

    std::string text = "name: ";
    text += scene.name;
    text += "\nscreen: " + std::to_string(scene.w) + "x" + std::to_string(scene.h);
    text += "\nsheet: x=" + std::to_string(static_cast<int>(box.x)) +
            " y=" + std::to_string(static_cast<int>(box.y)) +
            " w=" + std::to_string(static_cast<int>(box.w)) +
            " h=" + std::to_string(static_cast<int>(box.h));
    text += "\nsection: " + std::string(views::settingsSectionName(state.settingsSheet.section));
    text += " search: '" + std::string(state.settingsSheet.search.data()) + "'";
    text += "\nrows: built=" + std::to_string(state.settingsSheet.rowsBuilt) +
            " drawn=" + std::to_string(state.settingsSheet.rowsDrawn) +
            " selected=" + std::to_string(state.settingsSheet.selected);
    // The three fills that carry meaning: the active section's inversion, the selected row's mark
    // and an ordinary row's surface. A cell renderer states a fill as a palette index, and the
    // selection's own `selection` fill quantizes onto the surface - which is why the selected row's
    // *mark* cell is the one probed (it must not read as the surface).
    {
        // The active chip: `x0`, then every earlier chip's width (`text + 2 * gu`) and gap
        // (`2 * gu`) - the strip's own arithmetic, in cells.
        int stripX = static_cast<int>(box.x) + 1;
        for (int i = 0; i < state.settingsSheet.section; ++i)
            stripX += ui::displayWidth(views::settingsSectionName(i)) + 4;
        stripX += 1;
        const int stripY = static_cast<int>(box.y) + 3;
        const int firstRowY = static_cast<int>(box.y) + 4;
        const int markX = static_cast<int>(box.x) + 1; // the frame is one unit wide
        const int selectedY = firstRowY + state.settingsSheet.selected;
        const int plainY = selectedY + 1;
        text += "\nmarks: strip=" + std::to_string(grid.bgAt(stripX, stripY)) +
                " selected=" + std::to_string(grid.fgAt(markX, selectedY)) +
                " row=" + std::to_string(grid.fgAt(markX, plainY));
    }
    text += "\ntext:\n";
    for (int y = 0; y < grid.ny; ++y) {
        text += trimTrailingSpaces(grid.rowText(y));
        text += '\n';
    }
    return text;
}

void checkGoldens(bool update) {
    g_phase = "G: goldens";
    const std::string dir = TEST_SETTINGS_GOLDEN_DIR;
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

    // One glyph set whatever machine runs this: the ASCII column would change every box-drawing
    // cell a golden pins (`LLMFUN_ASCII=0` outranks TERM and the locale).
    ::setenv("LLMFUN_ASCII", "0", 1);

    ImGui::CreateContext();
    ImTui_ImplText_Init();

    checkSource();
    checkCatalogue();
    checkRows();
    checkKeys();
    checkClicks();
    checkFrameWiring();
    checkPersistence();
    checkCopy();
    checkGoldens(update);

    ImGui::DestroyContext();

    if (g_failures == 0) {
        std::printf("test_settings_sheet: %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "test_settings_sheet: %d of %d checks failed\n", g_failures, g_checks);
    return 1;
}
