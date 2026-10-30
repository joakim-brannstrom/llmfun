// probe_chords.cpp — chord-evidence probe for the key gate (task 2).
//
// Starts a production backend with an InputTextMultiline active and injects one
// chord per measurement window, then reports what actually reached ImGui:
// ImGui::IsKeyPressed() for every key the MVP may bind, io.KeyShift/KeyCtrl/
// KeyAlt at the press frame, and io.InputQueueCharacters (what the chord typed
// into the field).
//
// Text mode drives the real ncurses backend on a PTY owned by this process, so
// the terminal bytes are delivered exactly as a terminal would deliver them and
// the frame loop is deterministic (no sleeps, no feeder child). GUI mode drives
// the real GLFW/OpenGL3 backend and injects at the ImGui IO boundary in the same
// order imgui_impl_glfw uses (modifiers, then the key pair).
//
// Record per chord (exact format; stderr, because stdout is the terminal under
// test in text mode):
//     chord=%s term=%s keyPressed=%d shift=%d ctrl=%d alt=%d chars=[%s]
// keyPressed = number of distinct keys pressed in that window (0 = the chord
// produced no ImGui key event at all). shift/ctrl/alt = io.Key* at the press of
// the chord's principal key (or at the first press when the principal key never
// fires). chars = every character the chord queued, escaped (\xNN, \n, \[, \],
// \\).
//
// A "# detail ..." line follows each record with the ordered key list, whether
// the chord's principal key actually arrived (principal=0|1 — the record's
// keyPressed can be >0 because of a *different* key, e.g. Alt+1's Escape), the
// frame the first key arrived on, the window's wall time in ms, the field's
// active state before/after, and the field's text at the end of the window.
// Exit status is about the harness only: non-zero means the field never took
// focus, the sanity chord ("a") was not delivered, or --expect saw a row that
// does not match the recorded table — never that a chord is undelivered.
//
// Usage:
//   ./probe_chords [--term NAME] [--size WxH] [--window FRAMES] [--csiu] [--expect]
//   xvfb-run -s "-screen 0 1280x820x24" ./probe_chords --gui [--expect]
//
// --term selects $TERM for the ncurses backend (default: $TERM, else
// xterm-256color). --csiu feeds the CSI-u form instead of the legacy bytes, to
// measure what a CSI-u terminal (kitty/wezterm/foot opt-in protocol) would get;
// no kitty binary exists in this image, so that mode is supplementary evidence.
// --window is the number of frames a chord gets to arrive (default 12).
// --expect compares every record against the table in plan/key_gate.md (the
// regression guard task 19 can run in CI); it exits 1 on the first run whose
// measurement differs, e.g. after a backend or imgui upgrade.
#include "imtui/imtui.h"

#include "imgui/imgui_internal.h" // ClearActiveID(): deactivate the field between windows

#include "tui_backend.h"

#include <array>
#include <chrono>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace {

using llmfun::tui::Backend;
using llmfun::tui::BackendKind;

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

std::string utf8Of(unsigned int cp) {
    std::string out;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | ((cp >> 18) & 0x07));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return out;
}

// Escapes a byte string so a record stays one line and stays parseable: ']' and
// '\', ']' and '[' are backslash-escaped, control bytes become \xNN, newline
// becomes \n, so a record stays one line inside its [...] field.
std::string escapeBytes(const std::string& in) {
    std::string out;
    for (unsigned char c : in) {
        if (c == '\\' || c == ']' || c == '[') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c >= 32 && c < 127) {
            out += static_cast<char>(c);
        } else {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02X", c);
            out += buf;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Chord table
// ---------------------------------------------------------------------------

struct Row {
    const char* name;              // matrix row, also the record's chord= value
    std::string bytes;             // text mode: the bytes the terminal sends
    ImGuiKey key;                  // the chord's principal key (record + GUI injection)
    int mods;                      // ImGuiMod_* the chord is supposed to carry (GUI)
    const char* prefill = nullptr; // text present when the field is activated
    bool dirty = false;            // GUI: inject one character before the chord
    /// Text mode, TERM=linux: the bytes for a key whose encoding is
    /// terminfo-driven (only `End` today: xterm's `kend=\EOF` vs linux's
    /// `kend=\E[4~`). Empty = the same bytes as `bytes`.
    std::string bytesLinux;
};

// Legacy encodings. Provenance: the bytes a terminfo-driven terminal emulator
// writes to the pty for these keys (measured with `tmux send-keys` into
// TERM=xterm-256color and TERM=linux panes; both produce the same bytes), which
// is also what the xterm terminfo entry can express — it has no capability for
// Shift+Enter, Ctrl+Enter, Ctrl+digit, Ctrl+Shift+letter or Alt+digit, so those
// collapse onto the plain key's byte.
std::vector<Row> legacyRows() {
    return {
        {"Enter", "\r", ImGuiKey_Enter, 0},
        {"Shift+Enter", "\r", ImGuiKey_Enter, ImGuiMod_Shift},
        {"Ctrl+Enter", "\r", ImGuiKey_Enter, ImGuiMod_Ctrl},
        {"Ctrl+1", "1", ImGuiKey_1, ImGuiMod_Ctrl},
        {"Ctrl+2", std::string("\0", 1), ImGuiKey_2, ImGuiMod_Ctrl},
        {"Ctrl+5", "\x1d", ImGuiKey_5, ImGuiMod_Ctrl},
        {"Ctrl+6", "\x1e", ImGuiKey_6, ImGuiMod_Ctrl},
        {"Ctrl+Shift+L", "\x0c", ImGuiKey_L, ImGuiMod_Ctrl | ImGuiMod_Shift},
        {"Alt+1",
         "\x1b"
         "1",
         ImGuiKey_1, ImGuiMod_Alt},
        // Supplementary: bound by the palette/quit tasks (10, 11) and by D-ESC.
        {"Escape", "\x1b", ImGuiKey_Escape, 0},
        {"Ctrl+K", "\x0b", ImGuiKey_K, ImGuiMod_Ctrl},
        {"Ctrl+Q", "\x11", ImGuiKey_Q, ImGuiMod_Ctrl},
        // Supplementary: End is bound as ScrollEnd (task 11). It is the one chord
        // whose bytes are driven by terminfo - xterm's `kend=\EOF` vs linux's
        // `kend=\E[4~` - so the row carries both and runWindow picks per --term.
        // What matters for the binding is that it arrives as the *End key* and not
        // as `Esc` + the literal `[4~`, which is what the Up/Down rows do.
        {"End", "\x1bOF", ImGuiKey_End, 0, nullptr, false, "\x1b[4~"},
        // Task 25's LoadEarlier pair. `PgUp` is the scroller's own key - a
        // terminfo-driven CSI sequence, like End - and `Ctrl+U` is the design's
        // abbreviation (0x15, inside the Ctrl+letter path's 1-26 range).
        // plan/key_gate.md §4e records what they measured.
        {"PgUp", "\x1b[5~", ImGuiKey_PageUp, 0},
        {"Ctrl+U", "\x15", ImGuiKey_U, ImGuiMod_Ctrl},
        // Task 26's copy key. A printable key never arrives as a key event in the
        // terminal (F2): the row measures what it *does* arrive as - one character.
        {"y", "y", ImGuiKey_Y, 0},
        // Task 45's feedback answer (`x` clears an unanswered warning or error). The same
        // printable-key delivery as `y`: one character in the grid, a key event in the GUI.
        {"x", "x", ImGuiKey_X, 0},
        // Supplementary: chords later tasks condition on (29 Ctrl+I, 43 Ctrl+Shift+M,
        // 29/43 [ and ], 42 ?, 10/11 /). The control bytes that alias a special
        // key (9/10/13/27) and the printable keys are the ones that collapse.
        {"Ctrl+I", "\x09", ImGuiKey_I, ImGuiMod_Ctrl},
        {"Ctrl+J", "\x0a", ImGuiKey_J, ImGuiMod_Ctrl},
        {"Ctrl+M", "\x0d", ImGuiKey_M, ImGuiMod_Ctrl},
        {"Ctrl+[", "\x1b", ImGuiKey_LeftBracket, ImGuiMod_Ctrl},
        {"Ctrl+Shift+M", "\x0d", ImGuiKey_M, ImGuiMod_Ctrl | ImGuiMod_Shift},
        {"[", "[", ImGuiKey_LeftBracket, 0},
        {"]", "]", ImGuiKey_RightBracket, 0},
        {"?", "?", ImGuiKey_Slash, ImGuiMod_Shift},
        {"/", "/", ImGuiKey_Slash, 0},
        // Task 43's focus-mode key. `F11` is a function key: xterm and the linux console both send
        // `ESC[23~` for it, and the ncurses backend's `ncurses_keycode_to_imgui()` has no
        // function-key case at all - so the measurement is what a real pty does with the bytes. The
        // grid's path to the same action is the palette's `Focus mode` row (plan/key_gate.md §10).
        {"F11", "\x1b[23~", ImGuiKey_F11, 0},
        // D-ESC evidence: Esc over a field that was activated with text in it.
        // Text mode types 'x' and then a lone Esc (the Esc byte is the same in
        // every CSI-u mode, so one row covers both).
        {"Escape-after-edit", "x\x1b", ImGuiKey_Escape, 0, "draft"},
    };
}

// CSI-u form: ESC [ codepoint ; 1+modifier-bits u (shift=1, alt=2, ctrl=4).
// Supplementary: no kitty/CSI-u terminal is installed in this image.
std::vector<Row> csiuRows() {
    return {
        {"Enter", "\r", ImGuiKey_Enter, 0},
        {"Shift+Enter", "\x1b[13;2u", ImGuiKey_Enter, ImGuiMod_Shift},
        {"Ctrl+Enter", "\x1b[13;5u", ImGuiKey_Enter, ImGuiMod_Ctrl},
        {"Ctrl+1", "\x1b[49;5u", ImGuiKey_1, ImGuiMod_Ctrl},
        {"Ctrl+2", "\x1b[50;5u", ImGuiKey_2, ImGuiMod_Ctrl},
        {"Ctrl+5", "\x1b[53;5u", ImGuiKey_5, ImGuiMod_Ctrl},
        {"Ctrl+6", "\x1b[54;5u", ImGuiKey_6, ImGuiMod_Ctrl},
        {"Ctrl+Shift+L", "\x1b[108;6u", ImGuiKey_L, ImGuiMod_Ctrl | ImGuiMod_Shift},
        {"Alt+1", "\x1b[49;3u", ImGuiKey_1, ImGuiMod_Alt},
        // Supplementary, CSI-u encoded (codepoint ; 1+mods u).
        {"Escape", "\x1b[27u", ImGuiKey_Escape, 0},
        {"Ctrl+K", "\x1b[107;5u", ImGuiKey_K, ImGuiMod_Ctrl},
        {"Ctrl+Q", "\x1b[113;5u", ImGuiKey_Q, ImGuiMod_Ctrl},
        // Unmodified printable keys keep their text encoding in a CSI-u
        // terminal (the protocol only encodes modified/ambiguous keys).
        {"Ctrl+I", "\x1b[105;5u", ImGuiKey_I, ImGuiMod_Ctrl},
        {"Ctrl+J", "\x1b[106;5u", ImGuiKey_J, ImGuiMod_Ctrl},
        {"Ctrl+M", "\x1b[109;5u", ImGuiKey_M, ImGuiMod_Ctrl},
        {"Ctrl+[", "\x1b[91;5u", ImGuiKey_LeftBracket, ImGuiMod_Ctrl},
        {"Ctrl+Shift+M", "\x1b[109;6u", ImGuiKey_M, ImGuiMod_Ctrl | ImGuiMod_Shift},
        {"[", "[", ImGuiKey_LeftBracket, 0},
        {"]", "]", ImGuiKey_RightBracket, 0},
        {"?", "?", ImGuiKey_Slash, ImGuiMod_Shift},
        {"/", "/", ImGuiKey_Slash, 0},
        {"Escape-after-edit", "x\x1b", ImGuiKey_Escape, 0, "draft"},
    };
}

std::vector<Row> guiRows() {
    return {
        {"Enter", "", ImGuiKey_Enter, 0},
        {"Shift+Enter", "", ImGuiKey_Enter, ImGuiMod_Shift},
        {"Ctrl+Enter", "", ImGuiKey_Enter, ImGuiMod_Ctrl},
        {"Ctrl+1", "", ImGuiKey_1, ImGuiMod_Ctrl},
        {"Ctrl+2", "", ImGuiKey_2, ImGuiMod_Ctrl},
        {"Ctrl+5", "", ImGuiKey_5, ImGuiMod_Ctrl},
        {"Ctrl+6", "", ImGuiKey_6, ImGuiMod_Ctrl},
        {"Ctrl+Shift+L", "", ImGuiKey_L, ImGuiMod_Ctrl | ImGuiMod_Shift},
        {"Alt+1", "", ImGuiKey_1, ImGuiMod_Alt},
        // Supplementary: bound by the palette/quit tasks (10, 11) and by D-ESC.
        {"Escape", "", ImGuiKey_Escape, 0},
        {"Ctrl+K", "", ImGuiKey_K, ImGuiMod_Ctrl},
        {"Ctrl+Q", "", ImGuiKey_Q, ImGuiMod_Ctrl},
        // Supplementary: End, the ScrollEnd binding (task 11).
        {"End", "", ImGuiKey_End, 0},
        // Task 25's LoadEarlier pair.
        {"PgUp", "", ImGuiKey_PageUp, 0},
        {"Ctrl+U", "", ImGuiKey_U, ImGuiMod_Ctrl},
        // Task 26's copy key (see the legacy row).
        {"y", "", ImGuiKey_Y, 0},
        // Task 45's feedback answer (see the legacy row).
        {"x", "", ImGuiKey_X, 0},
        {"Ctrl+I", "", ImGuiKey_I, ImGuiMod_Ctrl},
        {"Ctrl+J", "", ImGuiKey_J, ImGuiMod_Ctrl},
        {"Ctrl+M", "", ImGuiKey_M, ImGuiMod_Ctrl},
        {"Ctrl+[", "", ImGuiKey_LeftBracket, ImGuiMod_Ctrl},
        {"Ctrl+Shift+M", "", ImGuiKey_M, ImGuiMod_Ctrl | ImGuiMod_Shift},
        {"[", "", ImGuiKey_LeftBracket, 0},
        {"]", "", ImGuiKey_RightBracket, 0},
        {"?", "", ImGuiKey_Slash, ImGuiMod_Shift},
        {"/", "", ImGuiKey_Slash, 0},
        // Task 43's focus-mode key (see the legacy row). The GUI delivers it: the injection below
        // is the one `imgui_impl_glfw`'s key callback makes for a function key, so what this proves
        // is that nothing in the UI's own input path drops it.
        {"F11", "", ImGuiKey_F11, 0},
        {"Escape-after-edit", "", ImGuiKey_Escape, 0, "draft", /*dirty=*/true},
    };
}

// ---------------------------------------------------------------------------
// Recorded expectations (--expect). Values are the measurements in
// plan/key_gate.md; they must be updated together with the artifact, never to
// make a failing run pass.
// ---------------------------------------------------------------------------

struct Expectation {
    const char* name;
    int presses;
    int shift;
    int ctrl;
    int alt;
    bool principal;
    bool activeAfter;
    const char* chars;
};

const Expectation kLegacyExpectations[] = {
    {"Enter", 1, 0, 0, 0, true, true, ""},
    {"Shift+Enter", 1, 0, 0, 0, true, true, ""},
    {"Ctrl+Enter", 1, 0, 0, 0, true, true, ""},
    {"Ctrl+1", 0, 0, 0, 0, false, true, "1"},
    {"Ctrl+2", 0, 0, 0, 0, false, true, ""},
    {"Ctrl+5", 0, 0, 0, 0, false, true, ""},
    {"Ctrl+6", 0, 0, 0, 0, false, true, ""},
    {"Ctrl+Shift+L", 1, 0, 1, 0, true, true, ""},
    {"Alt+1", 1, 0, 0, 0, false, false, "1"},
    {"Escape", 1, 0, 0, 0, true, false, ""},
    {"Ctrl+K", 1, 0, 1, 0, true, true, ""},
    {"Ctrl+Q", 1, 0, 1, 0, true, true, ""}, // delivered since task 11 cleared IXON (§4a)
    {"End", 1, 0, 0, 0, true, true, ""},
    {"PgUp", 1, 0, 0, 0, true, true, ""},
    {"Ctrl+U", 1, 0, 1, 0, true, true, ""},
    {"y", 0, 0, 0, 0, false, true, "y"},
    {"x", 0, 0, 0, 0, false, true, "x"},
    {"Ctrl+I", 1, 0, 0, 0, false, true, ""},
    {"Ctrl+J", 1, 0, 0, 0, false, true, ""},
    {"Ctrl+M", 1, 0, 0, 0, false, true, ""},
    {"Ctrl+[", 1, 0, 0, 0, false, false, ""},
    {"Ctrl+Shift+M", 1, 0, 0, 0, false, true, ""},
    {"[", 0, 0, 0, 0, false, true, "["},
    {"]", 0, 0, 0, 0, false, true, "]"},
    {"?", 0, 0, 0, 0, false, true, "?"},
    {"/", 0, 0, 0, 0, false, true, "/"},
    // Task 43: `F11` never reaches the app in the terminal - ncurses recognises the `ESC[23~` key
    // and `ncurses_keycode_to_imgui()` maps no function key, so no key event and no character
    // arrives at all.
    {"F11", 0, 0, 0, 0, false, true, ""},
    {"Escape-after-edit", 1, 0, 0, 0, true, false, "x"},
};

const Expectation kCsiuExpectations[] = {
    {"Enter", 1, 0, 0, 0, true, true, ""},
    {"Shift+Enter", 1, 0, 0, 0, false, false, "[13;2u"},
    {"Ctrl+Enter", 1, 0, 0, 0, false, false, "[13;5u"},
    {"Ctrl+1", 1, 0, 0, 0, false, false, "[49;5u"},
    {"Ctrl+2", 1, 0, 0, 0, false, false, "[50;5u"},
    {"Ctrl+5", 1, 0, 0, 0, false, false, "[53;5u"},
    {"Ctrl+6", 1, 0, 0, 0, false, false, "[54;5u"},
    {"Ctrl+Shift+L", 1, 0, 0, 0, false, false, "[108;6u"},
    {"Alt+1", 1, 0, 0, 0, false, false, "[49;3u"},
    {"Escape", 1, 0, 0, 0, true, false, "[27u"},
    {"Ctrl+K", 1, 0, 0, 0, false, false, "[107;5u"},
    {"Ctrl+Q", 1, 0, 0, 0, false, false, "[113;5u"},
    {"Ctrl+I", 1, 0, 0, 0, false, false, "[105;5u"},
    {"Ctrl+J", 1, 0, 0, 0, false, false, "[106;5u"},
    {"Ctrl+M", 1, 0, 0, 0, false, false, "[109;5u"},
    {"Ctrl+[", 1, 0, 0, 0, false, false, "[91;5u"},
    {"Ctrl+Shift+M", 1, 0, 0, 0, false, false, "[109;6u"},
    {"[", 0, 0, 0, 0, false, true, "["},
    {"]", 0, 0, 0, 0, false, true, "]"},
    {"?", 0, 0, 0, 0, false, true, "?"},
    {"/", 0, 0, 0, 0, false, true, "/"},
    {"Escape-after-edit", 1, 0, 0, 0, true, false, "x"},
};

const Expectation kGuiExpectations[] = {
    {"Enter", 1, 0, 0, 0, true, true, ""},
    {"Shift+Enter", 1, 1, 0, 0, true, true, ""},
    {"Ctrl+Enter", 1, 0, 1, 0, true, false, ""},
    {"Ctrl+1", 1, 0, 1, 0, true, true, ""},
    {"Ctrl+2", 1, 0, 1, 0, true, true, ""},
    {"Ctrl+5", 1, 0, 1, 0, true, true, ""},
    {"Ctrl+6", 1, 0, 1, 0, true, true, ""},
    {"Ctrl+Shift+L", 1, 1, 1, 0, true, true, ""},
    {"Alt+1", 1, 0, 0, 1, true, true, ""},
    {"Escape", 1, 0, 0, 0, true, false, ""},
    {"Ctrl+K", 1, 0, 1, 0, true, true, ""},
    {"Ctrl+Q", 1, 0, 1, 0, true, true, ""},
    {"End", 1, 0, 0, 0, true, true, ""},
    {"PgUp", 1, 0, 0, 0, true, true, ""},
    {"Ctrl+U", 1, 0, 1, 0, true, true, ""},
    {"y", 1, 0, 0, 0, true, true, ""},
    {"x", 1, 0, 0, 0, true, true, ""},
    {"Ctrl+I", 1, 0, 1, 0, true, true, ""},
    {"Ctrl+J", 1, 0, 1, 0, true, true, ""},
    {"Ctrl+M", 1, 0, 1, 0, true, true, ""},
    {"Ctrl+[", 1, 0, 1, 0, true, true, ""},
    {"Ctrl+Shift+M", 1, 1, 1, 0, true, true, ""},
    {"[", 1, 0, 0, 0, true, true, ""},
    {"]", 1, 0, 0, 0, true, true, ""},
    {"?", 1, 1, 0, 0, true, true, ""},
    {"/", 1, 0, 0, 0, true, true, ""},
    {"F11", 1, 0, 0, 0, true, true, ""},
    {"Escape-after-edit", 1, 0, 0, 0, true, false, "x"},
};

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

struct Options {
    bool gui = false;
    bool csiu = false;
    bool expect = false;
    bool termGiven = false;
    bool sizeGiven = false;
    std::string term;
    int cols = 100;
    int rows = 30;
    int window = 12;
};

// ---------------------------------------------------------------------------
// PTY
// ---------------------------------------------------------------------------

struct Pty {
    int master = -1;
    std::string slavePath;
};

// Opens a pty pair and sets the pane size. The slave keeps the production line
// discipline: the backend only calls cbreak(), which clears ICANON/ECHO but
// leaves ISIG and IXON alone — so Ctrl+C stays a signal and Ctrl+Q/Ctrl+S are
// eaten by flow control before ncurses reads them.
bool openPty(const Options& opt, Pty& pty) {
    int master = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    if (master < 0)
        return false;
    int ptyNumber = 0;
    if (ioctl(master, TIOCGPTN, &ptyNumber) < 0) {
        close(master);
        return false;
    }
    int unlock = 0;
    if (ioctl(master, TIOCSPTLCK, &unlock) < 0) {
        close(master);
        return false;
    }
    std::array<char, 32> path{};
    std::snprintf(path.data(), path.size(), "/dev/pts/%d", ptyNumber);
    int slave = open(path.data(), O_RDWR | O_NOCTTY);
    if (slave < 0) {
        close(master);
        return false;
    }
    struct winsize ws{};
    ws.ws_row = static_cast<unsigned short>(opt.rows);
    ws.ws_col = static_cast<unsigned short>(opt.cols);
    if (ioctl(slave, TIOCSWINSZ, &ws) < 0) {
        std::fprintf(stderr, "probe: TIOCSWINSZ failed: %s\n", std::strerror(errno));
        close(slave);
        close(master);
        return false;
    }
    close(slave);
    pty.master = master;
    pty.slavePath = path.data();
    return true;
}

// Writes the whole chord; a short write would split the bytes across frames and
// silently change the measurement.
bool injectBytes(int master, const std::string& bytes) {
    size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t n = write(master, bytes.data() + written, bytes.size() - written);
        if (n <= 0) {
            std::fprintf(stderr, "probe: pty write failed: %s\n", std::strerror(errno));
            return false;
        }
        written += static_cast<size_t>(n);
    }
    return true;
}

// Discards the terminal's output so the pty buffer cannot fill and block the
// backend inside DrawScreen.
void drainPty(int master) {
    std::array<char, 65536> buf{};
    const int flags = fcntl(master, F_GETFL, 0);
    fcntl(master, F_SETFL, flags | O_NONBLOCK);
    while (read(master, buf.data(), buf.size()) > 0) {
    }
    fcntl(master, F_SETFL, flags);
}

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

// Candidate keys: every key the MVP might bind, including the printable keys the
// inspector/palette fallbacks use ([ ] / ?). Bracket/Digit/Letter ranges are
// contiguous in the ImGuiKey enum; the asserts fail a future imgui upgrade that
// breaks that instead of silently probing the wrong keys.
struct NamedKey {
    ImGuiKey key;
    const char* name;
};

const std::vector<NamedKey>& candidateKeys() {
    static_assert(ImGuiKey_9 - ImGuiKey_0 == 9, "ImGuiKey_0..9 must be contiguous");
    static_assert(ImGuiKey_Z - ImGuiKey_A == 25, "ImGuiKey_A..Z must be contiguous");
    static const std::vector<NamedKey> keys = [] {
        std::vector<NamedKey> v = {
            {ImGuiKey_Enter, "Enter"},
            {ImGuiKey_KeypadEnter, "KeypadEnter"},
            {ImGuiKey_Escape, "Escape"},
            {ImGuiKey_Tab, "Tab"},
            {ImGuiKey_Space, "Space"},
            {ImGuiKey_Backspace, "Backspace"},
            {ImGuiKey_LeftBracket, "["},
            {ImGuiKey_RightBracket, "]"},
            {ImGuiKey_Slash, "/"},
            {ImGuiKey_Backslash, "\\"},
            // End carries a binding (task 11, ScrollEnd), so the record has to be
            // able to see it; PgUp carries one since task 25 (LoadEarlier), and a key
            // the whitelist does not name reads as "nothing at all" - the instrument
            // bug the End row hit in task 11.
            {ImGuiKey_End, "End"},
            {ImGuiKey_PageUp, "PgUp"},
            // Task 43: `F11` is focus mode's key, so the record has to be able to see it. A key
            // this whitelist does not name reads as "nothing at all" - the instrument bug the End
            // row hit in task 11.
            {ImGuiKey_F11, "F11"},
        };
        for (int i = 0; i < 10; ++i)
            v.push_back({static_cast<ImGuiKey>(ImGuiKey_0 + i), nullptr});
        for (int i = 0; i < 26; ++i)
            v.push_back({static_cast<ImGuiKey>(ImGuiKey_A + i), nullptr});
        return v;
    }();
    return keys;
}

const char* keyName(ImGuiKey key) {
    for (const NamedKey& k : candidateKeys()) {
        if (k.key == key)
            return k.name != nullptr ? k.name : ImGui::GetKeyName(key);
    }
    return ImGui::GetKeyName(key);
}

struct Press {
    ImGuiKey key = ImGuiKey_None;
    int mods = 0;
};

struct Window {
    std::vector<Press> presses;
    std::string chars;
    std::string bufAtStart;
    std::string bufEnd;
    double ms = 0.0;
    double msMax = 0.0;
    int firstFrame = -1;
    bool activeBefore = false;
    bool activeAfter = false;
};

// What a record reports, derived once so report() and --expect agree.
struct Observed {
    int presses = 0;
    int shift = 0;
    int ctrl = 0;
    int alt = 0;
    bool principal = false; // did the chord's own key arrive?
    std::string chars;
};

class Harness {
public:
    Harness(Backend& backend, bool gui, Options opt, int master)
        : backend_(backend), gui_(gui), opt_(std::move(opt)), master_(master) {}

    const char* termLabel() const { return gui_ ? "gui" : opt_.term.c_str(); }

    // Runs the probe: sanity chord, then one window per row. Returns false on a
    // harness failure (field never focused, sanity chord not delivered) and, with
    // --expect, on any record that differs from the recorded table.
    bool run(const std::vector<Row>& rows) {
        // Settle: the first frames build the window and let the focus request
        // land. Discarded from every record.
        for (int i = 0; i < 3; ++i)
            frame(false, nullptr);
        std::fprintf(stderr, "# frame: display=%dx%d\n",
                     static_cast<int>(ImGui::GetIO().DisplaySize.x),
                     static_cast<int>(ImGui::GetIO().DisplaySize.y));

        // Sanity chord: a plain 'a'. In text mode the ncurses backend delivers
        // printable input as characters only (no key event); at the GUI IO
        // boundary the same key arrives as a key event and no character. Either
        // way, something must reach ImGui or the harness is measuring nothing.
        Row sanity{"sanity-a", "a", ImGuiKey_A, 0};
        Window sanityWindow;
        const bool sanityOk = runWindow(sanity, sanityWindow) &&
                              (gui_ ? !sanityWindow.presses.empty() : sanityWindow.chars == "a");
        if (!sanityOk) {
            std::fprintf(stderr,
                         "# selfcheck term=%s keys=[%s] chars=[%s] active=%d FAILED "
                         "(the harness delivered nothing; refusing to record)\n",
                         termLabel(), sanityWindow.presses.empty() ? "" : "A",
                         escapeBytes(sanityWindow.chars).c_str(),
                         sanityWindow.activeBefore ? 1 : 0);
            return false;
        }
        std::fprintf(stderr, "# selfcheck term=%s keys=[%s] chars=[%s] active=1 ok\n", termLabel(),
                     sanityWindow.presses.empty() ? "" : "A",
                     escapeBytes(sanityWindow.chars).c_str());

        bool allOk = true;
        std::vector<Window> windows;
        windows.reserve(rows.size());
        for (const Row& row : rows) {
            Window w;
            if (!runWindow(row, w))
                allOk = false;
            report(row, w);
            windows.push_back(std::move(w));
        }
        if (expect_ != nullptr) {
            if (!checkExpectations(rows, windows))
                allOk = false;
        }
        return allOk;
    }

    // Layout of the expected table (count must match the row list of the mode).
    void setExpectations(const Expectation* table, size_t count) {
        expect_ = table;
        expectCount_ = count;
    }

    // The bytes this row's key sends in this terminal: only a terminfo-driven key
    // (End) carries a second encoding, everything else sends the same bytes under
    // both TERMs.
    const std::string& chordBytes(const Row& row) const {
        if (!row.bytesLinux.empty() && opt_.term.find("linux") != std::string::npos)
            return row.bytesLinux;
        return row.bytes;
    }

    // One chord window: focus, verify focus, inject, sample `opt_.window` frames.
    bool runWindow(const Row& row, Window& out) {
        // Every window gets a fresh widget id and buffer: an InputText keeps its
        // edit state per id and would otherwise carry the previous chord's text
        // into this window (and ignore the reset).
        ++windowSerial_;
        label_ = "##composer_" + std::to_string(windowSerial_);
        buffer_.fill('\0');
        if (row.prefill != nullptr)
            std::snprintf(buffer_.data(), buffer_.size(), "%s", row.prefill);
        out.presses.clear();
        out.chars.clear();
        // SetKeyboardFocusHere() takes effect on the following frame, and
        // InputText skips key handling on the frame it was just activated
        // (imgui_widgets.cpp:5132): give the activation two frames to settle.
        for (int i = 0; i < 4; ++i)
            frame(/*assertFocus=*/i == 0, nullptr);
        out.bufAtStart = std::string(buffer_.data());
        out.activeBefore = fieldActive_;
        if (!fieldActive_)
            return false;

        if (gui_)
            injectGui(row);
        else if (!injectBytes(master_, chordBytes(row)))
            return false;

        // Frame 0 samples the freshly pumped input; the widget consumes
        // io.InputQueueCharacters inside the same frame, so sampling happens
        // before drawing. The ncurses backend caps the loop at 60 fps, so the
        // per-window total is ~constant; the slowest frame is what shows the
        // 25 ms escdelay after a lone Esc.
        double maxFrameMs = 0.0;
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < opt_.window; ++i) {
            const auto before = std::chrono::steady_clock::now();
            frame(/*assertFocus=*/false, &out);
            const double frameMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before)
                    .count();
            if (frameMs > maxFrameMs)
                maxFrameMs = frameMs;
        }
        out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                     .count();
        out.msMax = maxFrameMs;
        out.activeAfter = fieldActive_;
        out.bufEnd = std::string(buffer_.data());
        return true;
    }

private:
    // Backend pump, optional sampling, probe window, present. `sample` must run
    // before the widget: InputText consumes io.InputQueueCharacters.
    void frame(bool assertFocus, Window* sample) {
        backend_.newFrame();
        if (sample != nullptr)
            sampleNow(*sample);
        drawWindow(assertFocus);
        backend_.renderFrame();
        if (!gui_)
            drainPty(master_);
        ++frameIndex_;
    }

    void sampleNow(Window& out) {
        ImGuiIO& io = ImGui::GetIO();
        const int mods = (io.KeyShift ? ImGuiMod_Shift : 0) | (io.KeyCtrl ? ImGuiMod_Ctrl : 0) |
                         (io.KeyAlt ? ImGuiMod_Alt : 0);
        for (const NamedKey& candidate : candidateKeys()) {
            if (!ImGui::IsKeyPressed(candidate.key))
                continue;
            bool seen = false;
            for (const Press& p : out.presses)
                seen = seen || (p.key == candidate.key && p.mods == mods);
            if (!seen)
                out.presses.push_back(Press{candidate.key, mods});
            if (out.firstFrame < 0)
                out.firstFrame = frameIndex_;
        }
        for (ImWchar c : io.InputQueueCharacters)
            out.chars += utf8Of(static_cast<unsigned int>(c));
    }

    // The composer stand-in: a full-screen window holding one multiline field,
    // which is what every chord has to survive.
    void drawWindow(bool assertFocus) {
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("chord-evidence", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::TextUnformatted("chord probe: InputTextMultiline active");
        if (assertFocus)
            ImGui::SetKeyboardFocusHere();
        ImGui::InputTextMultiline(label_.c_str(), buffer_.data(), buffer_.size(),
                                  ImVec2(io.DisplaySize.x, 5.0f));
        fieldActive_ = ImGui::IsItemActive();
        ImGui::End();
    }

    // Same call sequence as ImGui_ImplGlfw_KeyCallback: modifiers first, then the
    // key down/up pair (imgui_impl_glfw.cpp:482-501). `dirty` rows type one
    // character first, standing in for the user having edited the field.
    void injectGui(const Row& row) {
        ImGuiIO& io = ImGui::GetIO();
        if (row.dirty)
            io.AddInputCharacter('x');
        const std::array<ImGuiKey, 3> modKeys = {ImGuiMod_Shift, ImGuiMod_Ctrl, ImGuiMod_Alt};
        for (ImGuiKey mod : modKeys)
            if (row.mods & mod)
                io.AddKeyEvent(mod, true);
        io.AddKeyEvent(row.key, true);
        io.AddKeyEvent(row.key, false);
        for (ImGuiKey mod : modKeys)
            if (row.mods & mod)
                io.AddKeyEvent(mod, false);
    }

    // Modifier state at the chord's principal key; if that key never arrived, at
    // the first key the chord did produce.
    static Observed observe(const Row& row, const Window& w) {
        Observed o;
        o.presses = static_cast<int>(w.presses.size());
        o.chars = w.chars;
        int mods = 0;
        bool found = false;
        for (const Press& p : w.presses) {
            if (p.key == row.key) {
                mods = p.mods;
                found = true;
                break;
            }
        }
        o.principal = found;
        if (!found && !w.presses.empty())
            mods = w.presses.front().mods;
        o.shift = (mods & ImGuiMod_Shift) ? 1 : 0;
        o.ctrl = (mods & ImGuiMod_Ctrl) ? 1 : 0;
        o.alt = (mods & ImGuiMod_Alt) ? 1 : 0;
        return o;
    }

    void report(const Row& row, const Window& w) {
        const Observed o = observe(row, w);
        std::string keys;
        for (const Press& p : w.presses) {
            if (!keys.empty())
                keys += ',';
            keys += keyName(p.key);
        }
        const std::string chars = escapeBytes(o.chars);
        std::fprintf(stderr, "chord=%s term=%s keyPressed=%d shift=%d ctrl=%d alt=%d chars=[%s]\n",
                     row.name, termLabel(), o.presses, o.shift, o.ctrl, o.alt, chars.c_str());
        std::fprintf(stderr,
                     "# detail chord=%s term=%s active=%d after=%d principal=%d keys=[%s] "
                     "firstFrame=%d ms=%.1f msMax=%.1f chars=[%s] bufStart=[%s] buf=[%s]\n",
                     row.name, termLabel(), w.activeBefore ? 1 : 0, w.activeAfter ? 1 : 0,
                     o.principal ? 1 : 0, keys.c_str(), w.firstFrame, w.ms, w.msMax, chars.c_str(),
                     escapeBytes(w.bufAtStart).c_str(), escapeBytes(w.bufEnd).c_str());
    }

    // Compares the run against the recorded table (--expect) and reports every
    // field that moved, so a backend/imgui change cannot silently rewrite the gate.
    bool checkExpectations(const std::vector<Row>& rows, const std::vector<Window>& windows) {
        if (expectCount_ != rows.size()) {
            std::fprintf(stderr, "# expect: table has %zu rows but the probe ran %zu FAILED\n",
                         expectCount_, rows.size());
            return false;
        }
        int mismatches = 0;
        for (size_t i = 0; i < rows.size(); ++i) {
            const Expectation& e = expect_[i];
            const Observed o = observe(rows[i], windows[i]);
            const bool same = std::strcmp(e.name, rows[i].name) == 0 && e.presses == o.presses &&
                              e.shift == o.shift && e.ctrl == o.ctrl && e.alt == o.alt &&
                              e.principal == o.principal &&
                              e.activeAfter == windows[i].activeAfter &&
                              std::strcmp(e.chars, o.chars.c_str()) == 0;
            if (same)
                continue;
            ++mismatches;
            std::fprintf(stderr,
                         "# expect: chord=%s wanted keyPressed=%d shift=%d ctrl=%d alt=%d "
                         "principal=%d after=%d chars=[%s], got keyPressed=%d shift=%d ctrl=%d "
                         "alt=%d principal=%d after=%d chars=[%s]\n",
                         e.name, e.presses, e.shift, e.ctrl, e.alt, e.principal ? 1 : 0,
                         e.activeAfter ? 1 : 0, e.chars, o.presses, o.shift, o.ctrl, o.alt,
                         o.principal ? 1 : 0, windows[i].activeAfter ? 1 : 0, o.chars.c_str());
        }
        std::fprintf(stderr, "# expect: %d row(s) compared, %d mismatch(es) %s\n",
                     static_cast<int>(rows.size()), mismatches, mismatches == 0 ? "ok" : "FAILED");
        return mismatches == 0;
    }

    Backend& backend_;
    bool gui_ = false;
    Options opt_;
    int master_ = -1;
    std::array<char, 512> buffer_{};
    std::string label_{"##composer_0"};
    int windowSerial_ = 0;
    bool fieldActive_ = false;
    int frameIndex_ = 0;
    const Expectation* expect_ = nullptr;
    size_t expectCount_ = 0;
};
// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

// The GUI has no pty to size, so its row prints the backend default and the
// measured display size arrives with the "# frame:" line instead.
void printHeader(const Options& opt, const char* mode) {
    if (opt.gui) {
        std::fprintf(stderr, "# probe: mode=%s imgui=%s term=gui window=%d\n", mode, IMGUI_VERSION,
                     opt.window);
        return;
    }
    std::fprintf(stderr, "# probe: mode=%s imgui=%s term=%s size=%dx%d window=%d\n", mode,
                 IMGUI_VERSION, opt.term.c_str(), opt.cols, opt.rows, opt.window);
}

bool initImGui() {
    IMGUI_CHECKVERSION();
    ImGuiContext* context = ImGui::CreateContext();
    if (context == nullptr)
        return false;
    ImGui::GetIO().IniFilename = nullptr; // never write imgui.ini from a probe
    return true;
}

int runText(const Options& opt, const std::vector<Row>& rows) {
    // --term selects the terminal the backend initialises against; ncurses reads
    // it from the environment at initscr() time.
    setenv("TERM", opt.term.c_str(), 1);
    Pty pty;
    if (!openPty(opt, pty)) {
        std::fprintf(stderr, "probe: cannot open a pty\n");
        return 2;
    }
    if (!initImGui()) {
        std::fprintf(stderr, "probe: cannot create the ImGui context\n");
        close(pty.master);
        return 2;
    }
    // The text backend needs a tty before it starts: take over stdin/stdout with
    // the pty slave (both must be the same terminal; stderr stays the report).
    const int slave = open(pty.slavePath.c_str(), O_RDWR | O_NOCTTY);
    if (slave < 0) {
        std::fprintf(stderr, "probe: cannot open %s\n", pty.slavePath.c_str());
        ImGui::DestroyContext();
        close(pty.master);
        return 2;
    }
    if (dup2(slave, 0) < 0 || dup2(slave, 1) < 0) {
        std::fprintf(stderr, "probe: cannot attach the pty to stdio\n");
        close(slave);
        ImGui::DestroyContext();
        close(pty.master);
        return 2;
    }
    close(slave);

    // Font-atlas warm-up before the backend init (1.92 selects the legacy atlas
    // path only while RendererHasTextures is clear). This mirrors the earlier
    // scratch probes; the app additionally runs initMarkdownTextMode() here.
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags &= ~ImGuiBackendFlags_RendererHasTextures;
    {
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsAlpha8(&pixels, &width, &height);
    }

    std::string error;
    const std::unique_ptr<Backend> backend = llmfun::tui::makeNcursesBackend();
    if (!backend->init(error)) {
        std::fprintf(stderr, "probe: ncurses backend init failed: %s\n", error.c_str());
        ImGui::DestroyContext();
        close(pty.master);
        return 2;
    }

    printHeader(opt, opt.csiu ? "ncurses-csiu" : "ncurses");
    // The line discipline decides two of the rows: ISIG keeps Ctrl+C a signal,
    // and IXON eats Ctrl+Q (XON) / Ctrl+S (XOFF) before the backend can read
    // them. ncurses clears ICRNL here, so '\r' reaches it as CR (which the
    // backend maps to Enter alongside '\n').
    struct termios tio{};
    if (tcgetattr(0, &tio) == 0) {
        std::fprintf(stderr, "# tty: icanon=%d echo=%d isig=%d icrnl=%d ixon=%d\n",
                     (tio.c_lflag & ICANON) ? 1 : 0, (tio.c_lflag & ECHO) ? 1 : 0,
                     (tio.c_lflag & ISIG) ? 1 : 0, (tio.c_iflag & ICRNL) ? 1 : 0,
                     (tio.c_iflag & IXON) ? 1 : 0);
    }
    std::fprintf(stderr, "# harness: backend=tui trickle=%d escdelay=25ms\n",
                 io.ConfigInputTrickleEventQueue ? 1 : 0);
    Harness harness(*backend, /*gui=*/false, opt, pty.master);
    if (opt.expect) {
        const Expectation* table = opt.csiu ? kCsiuExpectations : kLegacyExpectations;
        harness.setExpectations(table, opt.csiu ? std::size(kCsiuExpectations)
                                                : std::size(kLegacyExpectations));
    }
    const bool ok = harness.run(rows);
    backend->shutdown();
    ImGui::DestroyContext();
    close(pty.master);
    std::fprintf(stderr, "probe: %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}

int runGui(const Options& opt) {
    if (!initImGui()) {
        std::fprintf(stderr, "probe: cannot create the ImGui context\n");
        return 2;
    }
    llmfun::tui::TuiSettings settings; // no persisted file: the probe writes none
    std::string error;
    const std::unique_ptr<Backend> backend = llmfun::tui::makeGuiBackend("", settings);
    if (!backend->init(error)) {
        std::fprintf(stderr, "probe: gui backend init failed: %s\n", error.c_str());
        ImGui::DestroyContext();
        return 2;
    }
    printHeader(opt, "gui");
    std::fprintf(stderr,
                 "# harness: backend=gui inject=io (AddKeyEvent, imgui_impl_glfw ordering)\n");
    Harness harness(*backend, /*gui=*/true, opt, /*master=*/-1);
    if (opt.expect)
        harness.setExpectations(kGuiExpectations, std::size(kGuiExpectations));
    const bool ok = harness.run(guiRows());
    backend->shutdown();
    ImGui::DestroyContext();
    std::fprintf(stderr, "probe: %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}

bool parseSize(const char* text, Options& opt) {
    int cols = 0;
    int rows = 0;
    if (std::sscanf(text, "%dx%d", &cols, &rows) != 2 || cols < 20 || rows < 10)
        return false;
    opt.cols = cols;
    opt.rows = rows;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::setlocale(LC_ALL, "");
    Options opt;
    if (const char* env = std::getenv("TERM"); env != nullptr && env[0] != '\0')
        opt.term = env;
    else
        opt.term = "xterm-256color";

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--gui") == 0) {
            opt.gui = true;
        } else if (std::strcmp(arg, "--csiu") == 0) {
            opt.csiu = true;
        } else if (std::strcmp(arg, "--expect") == 0) {
            opt.expect = true;
        } else if (std::strcmp(arg, "--term") == 0 && i + 1 < argc) {
            opt.term = argv[++i];
            opt.termGiven = true;
        } else if (std::strcmp(arg, "--size") == 0 && i + 1 < argc) {
            if (!parseSize(argv[++i], opt)) {
                std::fprintf(stderr, "probe: --size wants WxH (>=20x10)\n");
                return 2;
            }
            opt.sizeGiven = true;
        } else if (std::strcmp(arg, "--window") == 0 && i + 1 < argc) {
            const int frames = std::atoi(argv[++i]);
            if (frames < 2 || frames > 600) {
                std::fprintf(stderr, "probe: --window wants 2..600 frames\n");
                return 2;
            }
            opt.window = frames;
        } else {
            std::fprintf(stderr,
                         "usage: %s [--term NAME] [--size WxH] [--window FRAMES] [--csiu] "
                         "[--gui] [--expect]\n",
                         argv[0]);
            return 2;
        }
    }
    // --term/--size/--csiu configure the pty and its bytes; --gui has no pty. Say
    // so instead of silently ignoring them (a wrong window size otherwise looks
    // like a measurement).
    if (opt.gui && (opt.csiu || opt.termGiven || opt.sizeGiven)) {
        std::fprintf(stderr, "probe: --term/--size/--csiu do not apply to --gui\n");
        return 2;
    }
    if (opt.gui)
        return runGui(opt);
    if (opt.csiu)
        return runText(opt, csiuRows());
    return runText(opt, legacyRows());
}
