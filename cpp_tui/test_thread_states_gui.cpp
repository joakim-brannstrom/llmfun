// test_thread_states_gui.cpp
//
// GUI (pixel-profile) golden screens for the three thread states of task 8
// (design 4.11 / FR-19): a new session, a session switch in flight and an
// unreachable backend.
//
// Why this binary exists: the text grid can be captured from the harness, but
// the pixel profile has no screenshot tool in the test image (no import, xwd,
// scrot, ffmpeg or magick) and the three states are not all reachable from the
// shipped binary (the offline flag is task 39's runtime status, the loading flag
// is a queued session switch). This test drives the real GUI backend through
// tuiRender - the same frame code the application runs - and reads each frame
// back through Backend::captureNextFramePpm(), so the write goes through the
// backend's own glReadPixels rather than an external tool. Q-05's "a real GUI
// golden" is closed by these PPM files; a human (or a future diff harness) can
// open them directly.
//
// What is asserted:
//   - every state renders a non-empty frame (the capture is bigger than a PPM
//     header, and the file re-reads with its own P6 header);
//   - the three captures differ from each other, so the picture depends on the
//     state and is not a constant background (a blank thread would make them
//     identical);
//   - the offline-with-thread case differs from the offline card, i.e. an
//     existing transcript is rendered instead of the card.
//
// Needs a display; runs headless under xvfb:
//   xvfb-run -s "-screen 0 1280x820x24" ./test_thread_states_gui --out DIR
//
// Exit codes: 0 = all assertions pass; 1 = assertion failure; 2 = setup error
// (no display / GUI backend init failed / bad arguments).

#include "model/thread_view.h"
#include "tui.h"
#include "tui_backend.h"
#include "tui_settings.h"
#include "ui/tokens.h"

#include "imgui/imgui.h"

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

const char* g_phase = "startup";

void fail(const std::string& what) {
    std::fprintf(stderr, "FAIL [%s]: %s\n", g_phase, what.c_str());
    std::exit(1);
}

void phase(const char* what) { g_phase = what; }

/// Everything a PPM needs: the header, the size and the pixel bytes.
struct Ppm {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgb;
};

bool readPpm(const std::string& path, Ppm& out) {
    FILE* in = std::fopen(path.c_str(), "rb");
    if (in == nullptr)
        return false;
    char magic[3] = {0, 0, 0};
    int width = 0;
    int height = 0;
    int maxValue = 0;
    const bool header = std::fscanf(in, "%2s %d %d %d", magic, &width, &height, &maxValue) == 4;
    // One whitespace byte separates the header from the raster.
    const int separator = header ? std::fgetc(in) : EOF;
    if (!header || std::strcmp(magic, "P6") != 0 || width <= 0 || height <= 0 || maxValue != 255 ||
        separator == EOF) {
        std::fclose(in);
        return false;
    }
    out.width = width;
    out.height = height;
    out.rgb.resize(static_cast<std::size_t>(width) * height * 3);
    const std::size_t read = std::fread(out.rgb.data(), 1, out.rgb.size(), in);
    std::fclose(in);
    return read == out.rgb.size();
}

} // namespace

int main(int argc, char** argv) {
    std::string outDir;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--out" && i + 1 < argc) {
            outDir = argv[++i];
            continue;
        }
        std::fprintf(stderr, "usage: %s --out DIR\n", argv[0]);
        return 2;
    }
    if (outDir.empty()) {
        std::fprintf(stderr, "usage: %s --out DIR\n", argv[0]);
        return 2;
    }

    phase("init");
    std::setlocale(LC_ALL, "");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    llmfun::tui::TuiState state;
    ImGui::GetIO().IniFilename = nullptr;

    // The window size is what the pixel-profile golden is taken at (D-GOLDEN:
    // 1280x820). It comes from the GUI settings file, so write one into the
    // output directory and let the backend load it; nothing is persisted
    // anywhere else and the shipped settings are never touched.
    const std::string iniPath = outDir + "/gui-golden.ini";
    llmfun::tui::TuiSettings view;
    view.fontScaleMain = 1.0f;
    view.windowWidth = 1280;
    view.windowHeight = 820;
    llmfun::tui::saveTuiSettings(view, iniPath);

    std::string error;
    auto backend = llmfun::tui::makeGuiBackend(iniPath, state.settings);
    if (!backend->init(error)) {
        std::fprintf(stderr, "GUI backend init failed (no display?): %s\n", error.c_str());
        ImGui::DestroyContext();
        return 2;
    }
    state.backend = std::move(backend);
    llmfun::tui::initMarkdownGuiMode(state);
    llmfun::tui::applyUiTheme();

    auto frames = [&state](int count) {
        for (int i = 0; i < count; ++i) {
            state.backend->newFrame();
            if (!llmfun::tui::tuiRender(state)) {
                std::fprintf(stderr, "%s: render asked to quit\n", g_phase);
                std::exit(1);
            }
            state.backend->renderFrame();
        }
    };

    /// Renders the current state, captures one frame into `path` and returns it.
    auto capture = [&](const std::string& path) {
        frames(2); // settle: the first frame after a state flip lays out the body
        if (!state.backend->captureNextFramePpm(path))
            fail("the GUI backend refused the capture request");
        frames(1);
        Ppm ppm;
        if (!readPpm(path, ppm))
            fail("the capture is not a readable P6 PPM: " + path);
        if (ppm.width != 1280 || ppm.height != 820)
            fail("capture is " + std::to_string(ppm.width) + "x" + std::to_string(ppm.height) +
                 ", expected 1280x820");
        return ppm;
    };

    // One rail fixture for every state: the state is the only difference.
    const auto railEntry = [](const char* id, const char* title, const char* preview,
                              std::size_t count, bool active) {
        llmfun::tui::SessionEntry entry;
        entry.id = id;
        entry.title = title;
        entry.preview = preview;
        entry.messageCount = count;
        entry.isActive = active;
        return entry;
    };
    state.sessionPanel.sessions = {
        railEntry("s001-0", "Alpha session", "first user preview", 3, true),
        railEntry("s002-1", "Beta session", "", 5, false),
        railEntry("s003-2", "Gamma session", "", 0, false),
    };
    state.sessionPanel.activeId = "s001-0";

    // --- Empty ---------------------------------------------------------------
    phase("empty");
    llmfun::tui::tuiClearOutput(state);
    state.offline = false;
    state.readyStatus = true;
    const Ppm empty = capture(outDir + "/gui-1280x820-empty.ppm");
    std::printf("empty: %dx%d, %zu bytes\n", empty.width, empty.height, empty.rgb.size());

    // --- Loading (a select sent, not yet acknowledged) ------------------------
    // The workbench's loading card is `deriveThreadView()`'s answer to "no messages and a
    // select in flight" (`RailState::loadingId`), not a queued switch behind a run - and the
    // card is a *still* skeleton: a shimmer would be motion for its own sake, and a card that
    // cannot animate cannot burn a frame budget either.
    phase("loading");
    state.readyStatus = false;
    llmfun::tui::tuiClearOutput(state);
    state.ui.rail.loadingId = "s002-1";
    const Ppm loading = capture(outDir + "/gui-1280x820-loading.ppm");
    std::printf("loading: %dx%d, %zu bytes\n", loading.width, loading.height, loading.rgb.size());
    // A second capture of the same state: the skeleton must not move.
    state.backend->captureNextFramePpm(outDir + "/gui-1280x820-loading-b.ppm");
    frames(9);
    Ppm shimmer;
    if (!readPpm(outDir + "/gui-1280x820-loading-b.ppm", shimmer))
        fail("the second loading capture is not a readable P6 PPM");
    if (shimmer.rgb != loading.rgb)
        fail("the loading card moved between two frames (it is a still skeleton)");
    if (loading.rgb == empty.rgb)
        fail("the loading card and the empty state draw the same pixels");
    state.readyStatus = true;
    state.ui.rail.loadingId.clear();

    // --- Offline card --------------------------------------------------------
    phase("offline");
    llmfun::tui::tuiClearOutput(state);
    state.offline = true;
    // Two captures of the same state: the card with the disabled composer's
    // tooltip (a nav-focused item reports a hover, which is the documented
    // tooltip) and the card alone, with the nav cursor hidden for that frame.
    const Ppm offline = capture(outDir + "/gui-1280x820-offline-tooltip.ppm");
    ImGui::SetNavCursorVisible(false);
    const Ppm offlineCard = capture(outDir + "/gui-1280x820-offline.ppm");
    ImGui::SetNavCursorVisible(true);
    std::printf("offline: %dx%d, %zu bytes\n", offlineCard.width, offlineCard.height,
                offlineCard.rgb.size());

    // --- Offline with a transcript: the card must not replace it -------------
    phase("offline-with-thread");
    llmfun::tui::ChatMessage msg;
    msg.summary = "hello";
    msg.text = "hello body";
    msg.type = llmfun::tui::ChatMessageType::User;
    msg.id = state.chat.outputLineNextId++;
    llmfun::tui::tuiAddOutputLine(state, msg);
    const Ppm withThread = capture(outDir + "/gui-1280x820-offline-thread.ppm");
    std::printf("offline-with-thread: %dx%d, %zu bytes\n", withThread.width, withThread.height,
                withThread.rgb.size());

    // --- The picture depends on the state ------------------------------------
    phase("assertions");
    if (empty.rgb == loading.rgb)
        fail("the empty and loading captures are identical (no state is being drawn)");
    if (empty.rgb == offline.rgb)
        fail("the empty and offline captures are identical (no state is being drawn)");
    if (offline.rgb == withThread.rgb)
        fail("the offline card and the offline-with-thread captures are identical "
             "(the card replaced the transcript)");

    // --- The block restyle (task 26) -----------------------------------------
    // One thread holding every message type the block renderer draws: a user query, a
    // final answer, a work group (an agent step with its thinking disclosure, a tool
    // call, its result) and a second group whose step is a code block. The text grid's
    // per-type goldens live in `test_blocks`; this is the pixel profile's half of the
    // same list, and the focused block is on purpose - the focus ring and the copy
    // control are part of the anatomy.
    phase("blocks");
    llmfun::tui::tuiClearOutput(state);
    state.offline = false;
    state.readyStatus = true;
    const auto blockMessage = [](size_t id, llmfun::tui::ChatMessageType type, const char* summary,
                                 const char* text, const char* thinking) {
        llmfun::tui::ChatMessage msg;
        msg.id = id;
        msg.type = type;
        msg.summary = summary;
        msg.text = text;
        msg.thinking = thinking;
        return msg;
    };
    llmfun::tui::tuiAddOutputLine(state, blockMessage(1, llmfun::tui::ChatMessageType::User,
                                                      "[1] how do I build it [1.2s]",
                                                      "how do I build it", ""));
    llmfun::tui::tuiAddOutputLine(state, blockMessage(2, llmfun::tui::ChatMessageType::FinalAnswer,
                                                      "[2] run make [3.4s]", "run make", ""));
    llmfun::tui::tuiAddOutputLine(
        state, blockMessage(3, llmfun::tui::ChatMessageType::Assistant, "[3] looking around [1.1s]",
                            "I will list the files", "reasoning about the tree"));
    llmfun::tui::tuiAddOutputLine(state, blockMessage(4, llmfun::tui::ChatMessageType::ToolCall,
                                                      "[4] tool call: bash ls [1.2s]",
                                                      "tool call: bash ls", ""));
    llmfun::tui::tuiAddOutputLine(
        state, blockMessage(5, llmfun::tui::ChatMessageType::ToolResponse,
                            "[5] tool result \xE2\x9C\x85 writeFile: 42 lines [1.3s]",
                            "tool result writeFile: 42 lines", ""));
    llmfun::tui::tuiAddOutputLine(state, blockMessage(6, llmfun::tui::ChatMessageType::System,
                                                      "[6] session resumed [0.4s]",
                                                      "session resumed", ""));
    llmfun::tui::tuiAddOutputLine(state, blockMessage(7, llmfun::tui::ChatMessageType::Assistant,
                                                      "[7] try this [1.0s]",
                                                      "```\nint main() { return 0; }\n```", ""));
    // The first group is the user query; focusing its row is a click's outcome without a
    // click (the state is plain data the view reads, and the harness may write it).
    // The first row's block, named the way the view names it: the user query's group,
    // message id and kind (the focus is an identity, not a row index).
    state.ui.thread.focus = llmfun::tui::model::BlockId{1, 1, llmfun::tui::model::BlockKind::User};
    const Ppm blocks = capture(outDir + "/gui-1280x820-blocks.ppm");
    std::printf("blocks: %dx%d, %zu bytes\n", blocks.width, blocks.height, blocks.rgb.size());
    // A broken capture (blank, all-black, or one flat colour) would pass the
    // "the three differ" checks only by accident; require a real palette.
    const auto distinctColours = [](const Ppm& ppm) {
        std::vector<bool> seen(1u << 15, false);
        std::size_t count = 0;
        for (std::size_t i = 0; i + 2 < ppm.rgb.size(); i += 3) {
            // 5 bits per channel is enough to tell a UI from a flat colour.
            const unsigned key = (static_cast<unsigned>(ppm.rgb[i] >> 3) << 10) |
                                 (static_cast<unsigned>(ppm.rgb[i + 1] >> 3) << 5) |
                                 static_cast<unsigned>(ppm.rgb[i + 2] >> 3);
            if (!seen[key]) {
                seen[key] = true;
                ++count;
            }
        }
        return count;
    };
    struct NamedShot {
        const char* name;
        const Ppm* ppm;
    };
    const NamedShot shots[] = {{"empty", &empty},
                               {"loading", &loading},
                               {"offline", &offline},
                               {"offline-with-thread", &withThread},
                               {"blocks", &blocks}};
    for (const NamedShot& shot : shots) {
        const std::size_t colours = distinctColours(*shot.ppm);
        if (colours < 8)
            fail(std::string("the ") + shot.name + " capture has only " + std::to_string(colours) +
                 " distinct colours: it is not a rendered UI");
        std::printf("%s: %zu distinct colours\n", shot.name, colours);
    }

    // The focus ring is a *painted* ring, not a state the drawing code believes in: the
    // block capture puts the focus on the first row, so the ring's own token has to be on
    // screen - and it has to be absent from a capture with no focused block.
    const ImVec4 ringColour = llmfun::tui::ui::color(llmfun::tui::ui::ColorToken::focusRing);
    const int ringR = int(ringColour.x * 255.0f + 0.5f);
    const int ringG = int(ringColour.y * 255.0f + 0.5f);
    const int ringB = int(ringColour.z * 255.0f + 0.5f);
    const auto ringPixels = [&](const Ppm& ppm) {
        int count = 0;
        for (std::size_t i = 0; i + 2 < ppm.rgb.size(); i += 3) {
            const int dr = int(ppm.rgb[i]) - ringR;
            const int dg = int(ppm.rgb[i + 1]) - ringG;
            const int db = int(ppm.rgb[i + 2]) - ringB;
            if (dr >= -3 && dr <= 3 && dg >= -3 && dg <= 3 && db >= -3 && db <= 3)
                ++count;
        }
        return count;
    };
    const int blocksRing = ringPixels(blocks);
    std::printf("focus ring: %d px in the block capture\n", blocksRing);
    // The ring is a *painted* ring, not a state the drawing code believes in: the block capture
    // puts the focus on the first row, so the ring's token has to be on screen. (The reverse
    // claim - "no focus, no ring" - has no clean witness in the workbench: the ring's colour and
    // the accent are near neighbours and the composer draws a ring of its own. `test_blocks`
    // pins the ring's identity in the grid instead, where the cells are exact.)
    if (blocksRing < 500)
        fail("the focused block's ring is not on screen (" + std::to_string(blocksRing) + " px)");

    state.backend->shutdown();
    state.backend.reset();
    ImGui::DestroyContext();
    // The settings file the backend loaded the golden's window size from is
    // scratch: shutdown() has just rewritten it with the final window size, so
    // it is removed here and only the captures remain.
    std::remove(iniPath.c_str());
    std::remove(llmfun::tui::settingsPath(iniPath).c_str());
    std::printf("OK: five GUI captures (four thread states + the block restyle) written to %s\n",
                outDir.c_str());
}
