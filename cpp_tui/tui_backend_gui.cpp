/// GUI backend behind the Backend seam: GLFW window + OpenGL3 renderer.
/// ImTui is bypassed entirely in GUI mode: stock Dear ImGui renders through
/// the vendored GLFW platform backend and OpenGL3 renderer backend.
#include "tui_backend.h"
#include "tui_chrome.h"
#include "tui_settings.h"
#include "ui/tokens.h"

#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_internal.h"
#include "imgui_rich_md/rich_md.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// The resize floor is the one thing glfw cannot report back (there is no limits
// getter), so the backend reads it from the window system instead. Both the glfw
// native accessors and X11 are resolved at run time: glfw3native.h pulls in
// Xrandr headers the library does not otherwise need, and a Wayland-only glfw
// exports no X11 accessor at all - neither may become a link dependency of every
// target that carries this library.
#if defined(__linux__) && defined(__has_include)
#if __has_include(<X11/Xlib.h>)
#define LLMFUN_HAVE_X11_HINTS 1
#endif
#endif

#if defined(LLMFUN_HAVE_X11_HINTS)
#include <X11/Xlib.h>
#include <X11/Xutil.h> // XSizeHints, PMinSize (WM_NORMAL_HINTS)

#include <dlfcn.h>
#endif

namespace llmfun::tui {

namespace {

// Last message captured by the GLFW error callback. GLFW reports errors on
// the thread that triggered them; the window path is single-threaded, so one
// process-wide string mirrors GLFW's own last-error semantics.
std::string glfwLastError;

void glfwErrorCallback(int error, const char* description) {
    glfwLastError = description != nullptr ? description : "unknown GLFW error";
    (void)error;
}

std::string lastGlfwErrorText() {
    return glfwLastError.empty() ? std::string{"(no GLFW error message)"} : glfwLastError;
}

/// The sink `tuiBackendSetWindowTitle()` forwards to (tui_backend.h, FR-02). GLFW copies the
/// string, so the reference lives for the call only; the window pointer is the one the
/// running backend created (one process, one window).
GLFWwindow* g_titleWindow = nullptr;

void applyWindowTitle(const std::string& title) {
    if (g_titleWindow != nullptr)
        glfwSetWindowTitle(g_titleWindow, title.c_str());
}

// Window geometry (defaults and the resize floor) lives in tui_chrome.h, shared
// with the window-defaults test and the token rework in task 12/20.
//
// The window opens with the seam's own name (`applicationWindowTitle()`, tui_backend.h): the
// title a fresh window carries and the title an empty session restores are one definition, not
// two literals that can drift apart.
// Idle pacing: wait for input or up to one 60 Hz period — never busy-poll.
constexpr double EventWaitSeconds = 1.0 / 60.0;

/// The operator's font size as a *drawn* size, 0 when it is unset or malformed.
///
/// `LLMFUN_GUI_FONT_SIZE` used to move the drawn size because the old code wrote
/// it into `ImGuiStyle::FontSizeBase` itself; since 1.92 that field is the only
/// thing that decides how large text draws, so the value has to be handed to
/// `ui::applyTypeScale()` - the one place that writes the base. Unset, empty,
/// non-numeric and out-of-range values answer 0 ("the type scale owns the size").
float configuredFontSize() {
    const char* env = std::getenv("LLMFUN_GUI_FONT_SIZE");
    if (env == nullptr || env[0] == '\0')
        return 0.0f;
    char* end = nullptr;
    const float px = std::strtof(env, &end);
    if (end == env || *end != '\0' || !(px > 0.0f) || px > 512.0f)
        return 0.0f;
    return px;
}

/// The size a font asset is *added* with. It is a raster hint, not the drawn size:
/// 1.92 bakes glyphs on demand at whatever `ImGuiStyle::FontSizeBase` asks for, and
/// the base comes from `ui::applyTypeScale()`.
///
/// 16 px is deliberately *not* the body step (15). An asset's size also becomes the
/// font's `LegacySize`, which is what `FontSizeBase` falls back to when nothing
/// wrote it - so a hint equal to the body step would make "the type scale was
/// applied" and "the base fell through from the font" indistinguishable, and a
/// deleted `applyTypeScale()` would keep every test green (the coincidence the
/// review flagged). With a different hint, `test_gui_window`'s base check fails the
/// moment the base stops coming from the scale.
constexpr float kFontRasterHintPx = 16.0f;
float fontAssetSize() { return kFontRasterHintPx; }

#if defined(LLMFUN_HAVE_X11_HINTS)
/// The answer to "does the window system hold the resize floor?".
enum class FloorProbe { Unobservable, Absent, Holds };

/// Look `symbol` up among the symbols the process already loaded. glfw is a
/// direct dependency of every GUI binary, so its native accessors are here
/// without a link-time reference (they are absent from a Wayland-only build).
void* loadedSymbol(const char* symbol) { return dlsym(RTLD_DEFAULT, symbol); }

/// Read the resize floor back out of the window system.
///
/// glfwSetWindowSizeLimits() has no getter: it only writes WM_NORMAL_HINTS, which
/// a window manager enforces on a drag. Those hints *are* readable, so the
/// backend can report an observation instead of a statement - a request that was
/// dropped from applySizeLimits() leaves no hints behind, and the window-defaults
/// test then sees the missing floor instead of a green suite.
FloorProbe probeSizeFloor(GLFWwindow* window, int minWidth, int minHeight) {
    using GetDisplayFn = void* (*)();
    using GetWindowFn = unsigned long (*)(GLFWwindow*);
    using GetHintsFn = int (*)(void*, unsigned long, XSizeHints*, long*);
    static const auto getDisplay =
        reinterpret_cast<GetDisplayFn>(loadedSymbol("glfwGetX11Display"));
    static const auto getWindow = reinterpret_cast<GetWindowFn>(loadedSymbol("glfwGetX11Window"));
    static const auto getHints = []() -> GetHintsFn {
        // RTLD_NOLOAD first: on an X11 platform glfw has the library open
        // already, and this must not add a second hold on it.
        void* x11 = dlopen("libX11.so.6", RTLD_LAZY | RTLD_NOLOAD);
        if (x11 == nullptr)
            x11 = dlopen("libX11.so.6", RTLD_LAZY);
        return x11 != nullptr ? reinterpret_cast<GetHintsFn>(dlsym(x11, "XGetWMNormalHints"))
                              : nullptr;
    }();
    if (getDisplay == nullptr || getWindow == nullptr || getHints == nullptr)
        return FloorProbe::Unobservable;
    Display* display = static_cast<Display*>(getDisplay());
    const unsigned long xwindow = window != nullptr ? getWindow(window) : 0;
    if (display == nullptr || xwindow == 0)
        return FloorProbe::Unobservable;
    XSizeHints hints{};
    long supplied = 0;
    if (getHints(display, xwindow, &hints, &supplied) == 0)
        return FloorProbe::Unobservable;
    return (hints.flags & PMinSize) != 0 && hints.min_width == minWidth &&
                   hints.min_height == minHeight
               ? FloorProbe::Holds
               : FloorProbe::Absent;
}
#endif

// Fonts: the UI chain (task 20), in order, each step reporting what won on stderr
// so a capture log records the typography it was made with:
//
//   1. LLMFUN_GUI_FONT      an operator's own TTF, size LLMFUN_GUI_FONT_SIZE
//                           (default: the body step). The existing escape hatch.
//   2. embedded Roboto      rich_md's asset table (IMGUI_RICHMD_EMBED_EXTRA_FILES
//                           in CMakeLists.txt). No asset outside the binary, no
//                           download, no new licence - Q-06 is settled as "keep
//                           Roboto for the MVP".
//   3. system sans          the first readable file of kSystemSansPaths: the
//                           fallback for a build that dropped the asset table.
//   4. bitmap pin           ImGui's own ProggyClean, which needs no asset at all,
//                           so the chain cannot end without a font.
//
// The pixel profile therefore never depends on the image having fonts installed
// (this one has none) and never depends on the network. Whatever the family, the
// *size* comes from the type scale (`ui::applyTypeScale()`), never from the
// font's own metrics.
constexpr const char* kSystemSansPaths[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", // Debian, Ubuntu
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",          // Fedora, RHEL
    "/usr/share/fonts/TTF/DejaVuSans.ttf",             // Arch
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
};

/// Steps 1 and 3. A missing or unreadable file is reported and falls through to the
/// next step instead of asserting (ImFontFlags_NoLoadError). The size is not part
/// of this line: the *drawn* size is printed once, by `logTypeBase()`.
bool addFileFont(const char* path, const char* why) {
    ImFontConfig cfg;
    cfg.Flags |= ImFontFlags_NoLoadError;
    if (ImGui::GetIO().Fonts->AddFontFromFileTTF(path, fontAssetSize(), &cfg) == nullptr)
        return false;
    std::fprintf(stderr, "ui: font=%s (%s)\n", path, why);
    return true;
}

/// One family out of rich_md's embedded asset table, or null when this build does
/// not carry it. The atlas owns the bytes for its whole lifetime and frees them
/// (FontDataOwnedByAtlas), so it gets its own copy.
ImFont* addEmbeddedFont(const ui::FontFace& face) {
    RichMd::AssetBytes bytes = RichMd::ReadAssetDefault(face.asset);
    if (!bytes || bytes->empty())
        return nullptr;
    ImFontConfig cfg;
    // The name labels the font in tooling and in this log; it is never a key.
    std::snprintf(cfg.Name, sizeof(cfg.Name), "%s %.0f", face.family, fontAssetSize());
    // The atlas owns the data for its whole lifetime and frees it
    // (FontDataOwnedByAtlas), so it gets its own copy of the bytes.
    void* data = IM_ALLOC(bytes->size());
    std::memcpy(data, bytes->data(), bytes->size());
    ImFont* font = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, static_cast<int>(bytes->size()),
                                                              fontAssetSize(), &cfg);
    if (font == nullptr) {
        IM_FREE(data);
        return nullptr;
    }
    return font;
}

/// Step 2. The embedded UI family. It becomes `io.FontDefault` - the family every
/// widget outside rich_md's own pushes draws with - and it is what lets the chain
/// work on an image with no fonts at all.
bool addEmbeddedUiFont() {
    ImFont* font = addEmbeddedFont(ui::uiFontFace());
    if (font == nullptr)
        return false;
    ImGui::GetIO().FontDefault = font;
    std::fprintf(stderr, "ui: font=%s (embedded)\n", ui::uiFontFamily());
    return true;
}

/// The code face: the same table, installed once and published to the token module
/// so `TypeStep::code` pushes it (`ui::setCodeFont`). A build without the asset
/// leaves that slot null, and a code step then keeps the UI family at 14 px.
void loadCodeFont() {
    ImFont* font = addEmbeddedFont(ui::monoFontFace());
    if (font == nullptr)
        return;
    ui::setCodeFont(font);
    std::fprintf(stderr, "ui: font=%s (embedded code)\n", ui::monoFontFamily());
}

/// The chain. Runs in `init()` before the first frame: the atlas is unlocked
/// there, and the frame loop must never add a font (a late add would bake at the
/// wrong size and re-render every glyph of that frame). It also installs the code
/// face, which is a second family rather than a step in this chain.
void configureUiFont() {
    const char* path = std::getenv("LLMFUN_GUI_FONT");
    if (path != nullptr && path[0] != '\0') {
        if (addFileFont(path, "LLMFUN_GUI_FONT")) {
            loadCodeFont();
            return;
        }
        std::fprintf(stderr, "LLMFUN_GUI_FONT: cannot load '%s'; trying the embedded font\n", path);
    }
    if (addEmbeddedUiFont()) {
        loadCodeFont();
        return;
    }
    for (const char* candidate : kSystemSansPaths)
        if (addFileFont(candidate, "system sans")) {
            loadCodeFont();
            return;
        }
    // The atlas must never be empty, and the vector default is the one ImGui's own
    // heuristic picks at the body step (it switches from the 13 px bitmap once the
    // resolved size reaches 15): pinning it keeps a no-asset build crisp instead of
    // stretching the bitmap 1.15x.
    ImGui::GetIO().Fonts->AddFontDefaultVector();
    loadCodeFont();
    std::fprintf(stderr, "ui: font=default vector (no asset, no system font)\n");
}

/// The size the profile actually draws at, printed once after the chain and
/// `ui::applyTypeScale()` have both run: this is the number a capture log has to
/// record, not the raster hint a font was added with.
void logTypeBase() {
    std::fprintf(stderr, "ui: type=%.0f px base (%s)\n", ImGui::GetStyle().FontSizeBase,
                 configuredFontSize() > 0.0f ? "LLMFUN_GUI_FONT_SIZE" : "body step");
}

// DPI: scale the style and the font by the monitor content scale, once, at
// init. ImGui_ImplGlfw_GetContentScaleForMonitor() reports 1.0 whenever the
// scale cannot be known (GLFW < 3.3 — the call is compiled out inside the
// vendored backend — and Wayland/macOS), so older GLFW keeps the unscaled
// defaults; the guard also skips the zero/negative values some virtual
// monitors report. FontScaleDpi is the 1.92 font scale (it replaced
// io.FontGlobalScale).
void applyDpiScale() {
    const float scale = ImGui_ImplGlfw_GetContentScaleForMonitor(glfwGetPrimaryMonitor());
    if (!(scale > 0.0f) || scale == 1.0f)
        return;
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleDpi = scale;
}

class GuiBackend final : public Backend {
public:
    explicit GuiBackend(std::string iniFilename, TuiSettings* settings)
        : iniFilename(std::move(iniFilename)), settings(settings) {}

    bool init(std::string& error) override {
        glfwLastError.clear();
        glfwSetErrorCallback(glfwErrorCallback);

        if (!glfwInit()) {
            error = "glfwInit failed: " + lastGlfwErrorText();
            return false;
        }

        // Missing or malformed values keep the built-in default.
        int width = DefaultGuiWindowWidth;
        int height = DefaultGuiWindowHeight;
        if (!iniFilename.empty()) {
            loadTuiSettings(*settings, iniFilename);
            if (settings->windowWidth > 0)
                width = settings->windowWidth;
            if (settings->windowHeight > 0)
                height = settings->windowHeight;
        }
        // A persisted size below the floor (hand-edited, or a file from an older
        // profile) opens at the floor: the limits set below stop an interactive
        // drag, they do not grow a window that was created underneath them.
        width = std::max(width, MinGuiWindowWidth);
        height = std::max(height, MinGuiWindowHeight);

        // attempt 1: GL 3.0 + GLSL 130; retry once: GL 2.1 + GLSL 120.
        // Each attempt passes its OWN pinned shader string — never nullptr.
        int attempt = 0;
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
        window = glfwCreateWindow(width, height, applicationWindowTitle(), nullptr, nullptr);
        if (window != nullptr) {
            attempt = 1;
        } else {
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
            window = glfwCreateWindow(width, height, applicationWindowTitle(), nullptr, nullptr);
            if (window != nullptr)
                attempt = 2;
        }
        if (window == nullptr) {
            error = "GLFW window creation failed (GL 3.0 and GL 2.1): " + lastGlfwErrorText();
            glfwTerminate();
            return false;
        }
        // Both attempts get the resize floor, and the size actually granted is
        // reported: a window manager may adjust the requested one, and the run
        // log is the evidence a headless capture cannot give (there is no
        // pointer and no window manager to drag with).
        applySizeLimits();
        int grantedWidth = 0;
        int grantedHeight = 0;
        glfwGetWindowSize(window, &grantedWidth, &grantedHeight);
        std::fprintf(stderr, "ui: window %dx%d (min %dx%d)\n", grantedWidth, grantedHeight,
                     MinGuiWindowWidth, MinGuiWindowHeight);

        glfwMakeContextCurrent(window);

        // Must not use V-SYNC. V-SYNC lead to the GUI thread locking in the
        // GLFW backend when the screensaver/lockscreen is activated in
        // wayland. It is OK with a bit of screen tearing in a GUI app because
        // the frames do not "differ" as much as in an FPS game.
        glfwSwapInterval(0);

        if (!ImGui_ImplGlfw_InitForOpenGL(window, true)) {
            error = "ImGui_ImplGlfw_InitForOpenGL failed";
            glfwDestroyWindow(window);
            window = nullptr;
            glfwTerminate();
            return false;
        }

        // One pinned shader version per attempt; the desktop auto-branch of a
        // nullptr argument could pick a version this context does not have.
        const char* glslVersion = attempt == 1 ? "#version 130" : "#version 120";
        if (!ImGui_ImplOpenGL3_Init(glslVersion)) {
            error = std::string{"ImGui_ImplOpenGL3_Init("} + glslVersion +
                    ") failed: OpenGL driver unavailable (bundled loader)";
            ImGui_ImplGlfw_Shutdown();
            glfwDestroyWindow(window);
            window = nullptr;
            glfwTerminate();
            return false;
        }

        // Font family (the chain in configureUiFont()), the type scale, the chrome
        // metrics and the one-time DPI adaptation of the style, before the first
        // frame. The size and the chrome metrics go in BEFORE ScaleAllSizes() so a
        // 2x monitor scales them too instead of leaving unscaled padding behind.
        configureUiFont();
        // The operator's `LLMFUN_GUI_FONT_SIZE` reaches the screen here and nowhere
        // else: the asset hint it also feeds cannot change the drawn size.
        ui::applyTypeScale(configuredFontSize());
        logTypeBase();
        applyGuiChromeMetrics();
        applyDpiScale();

        // The window exists and every fallible step is behind us: publish the title sink the
        // workbench's top bar mirrors the session title through (tui_backend.h). Doing it
        // here rather than at creation is what keeps a failed init from leaving a sink that
        // points at a destroyed window.
        g_titleWindow = window;
        setWindowTitleSink(&applyWindowTitle);

        return true;
    }

    void newFrame() override {
        // Event-waiting idle: block until input arrives or one period elapses
        // (vsync stays on), so a static window never busy-spins.
        glfwWaitEventsTimeout(EventWaitSeconds);
        ImGui_ImplGlfw_NewFrame();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        // Claim the Alt keys every frame: Dear ImGui's press-and-release-Alt
        // nav-layer toggle (NavUpdateWindowing) cannot tell a plain Alt tap
        // from AltGr - X11 reports no Ctrl for AltGr, so typing "@" with AltGr
        // cleared the active text input and jumped focus to the menu layer.
        // The app has no Alt chords, so owning the keys costs nothing.
        static const ImGuiID altOwner = ImHashStr("llmfun-tui-alt-claim");
        ImGui::SetKeyOwner(ImGuiMod_Alt, altOwner);
        ImGui::SetKeyOwner(ImGuiKey_LeftAlt, altOwner);
        ImGui::SetKeyOwner(ImGuiKey_RightAlt, altOwner);
    }

    void renderFrame() override {
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        // Before the swap: the finished frame is still the back buffer here.
        if (!pendingCapturePath.empty()) {
            const std::string path = pendingCapturePath;
            pendingCapturePath.clear();
            capturePpm(path);
        }
        glfwSwapBuffers(window);
    }

    void shutdown() override {
        // Persist the view settings with the final window size while the
        // window is still alive; the next start recreates the window with it.
        if (!iniFilename.empty() && window != nullptr) {
            int width = 0;
            int height = 0;
            glfwGetWindowSize(window, &width, &height);
            TuiSettings out = *settings;
            out.windowWidth = width;
            out.windowHeight = height;
            saveTuiSettings(out, iniFilename);
        }
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        // The sink must not outlive the window: a title arriving after the destroy would
        // write through a dangling pointer.
        setWindowTitleSink(nullptr);
        g_titleWindow = nullptr;
        glfwDestroyWindow(window);
        window = nullptr;
        glfwTerminate();
    }

    BackendKind kind() const override { return BackendKind::Gui; }

    bool shouldClose() const override { return window != nullptr && glfwWindowShouldClose(window); }

    bool captureNextFramePpm(const std::string& path) override {
        if (window == nullptr || path.empty())
            return false;
        pendingCapturePath = path;
        return true;
    }

    bool guiWindowGeometry(GuiWindowGeometry& geometry) const override {
        if (window == nullptr)
            return false;
        glfwGetWindowSize(window, &geometry.width, &geometry.height);
        geometry.minWidth = requestedMinWidth;
        geometry.minHeight = requestedMinHeight;
        geometry.limitsRequested = limitsRequested;
        geometry.limitsApplied = false;
        geometry.limitsObserved = false;
#if defined(LLMFUN_HAVE_X11_HINTS)
        const FloorProbe probe = limitsRequested
                                     ? probeSizeFloor(window, requestedMinWidth, requestedMinHeight)
                                     : FloorProbe::Unobservable;
        geometry.limitsObserved = probe != FloorProbe::Unobservable;
        geometry.limitsApplied = probe == FloorProbe::Holds;
#endif
        return geometry.width > 0 && geometry.height > 0;
    }

private:
    /// Ask the window system for the resize floor (D-GEOM). The pair is recorded
    /// *first* and then passed to glfw verbatim, so the numbers a reader of
    /// guiWindowGeometry() sees are the numbers that were requested - a stray
    /// literal in the call cannot diverge from the record. Whether the request
    /// landed is a separate question and is answered by reading the hints back
    /// (probeSizeFloor), not by this function.
    void applySizeLimits() {
        requestedMinWidth = MinGuiWindowWidth;
        requestedMinHeight = MinGuiWindowHeight;
        glfwSetWindowSizeLimits(window, requestedMinWidth, requestedMinHeight, GLFW_DONT_CARE,
                                GLFW_DONT_CARE);
        limitsRequested = true;
    }

    /// Read the finished frame back and write it as a binary PPM. Called from
    /// renderFrame() before the swap, so the default framebuffer still holds the
    /// frame that was just drawn.
    void capturePpm(const std::string& path) {
        // glReadPixels through GLFW's loader: this library does not link libGL
        // (the OpenGL3 backend dlopens the driver), so the symbol has to come
        // from the context that is current here.
        using ReadPixelsFn = void (*)(int, int, int, int, unsigned int, unsigned int, void*);
        auto readPixels = reinterpret_cast<ReadPixelsFn>(glfwGetProcAddress("glReadPixels"));
        if (readPixels == nullptr) {
            std::fprintf(stderr, "capture: glReadPixels is unavailable in this GL driver\n");
            return;
        }
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        if (width <= 0 || height <= 0) {
            std::fprintf(stderr, "capture: framebuffer is %dx%d\n", width, height);
            return;
        }
        // GL_RGBA/GL_UNSIGNED_BYTE (0x1908/0x1401): 4 bytes per pixel, so the
        // default 4-byte pack alignment needs no glPixelStorei - which this
        // translation unit could not call without the loader anyway.
        std::vector<unsigned char> rgba(static_cast<std::size_t>(width) * height * 4);
        readPixels(0, 0, width, height, 0x1908, 0x1401, rgba.data());

        FILE* out = std::fopen(path.c_str(), "wb");
        if (out == nullptr) {
            std::fprintf(stderr, "capture: cannot write %s\n", path.c_str());
            return;
        }
        std::fprintf(out, "P6\n%d %d\n255\n", width, height);
        // GL's origin is the bottom-left corner, a PPM's the top-left one.
        for (int y = height - 1; y >= 0; --y) {
            const unsigned char* row = rgba.data() + static_cast<std::size_t>(y) * width * 4;
            for (int x = 0; x < width; ++x)
                std::fwrite(row + static_cast<std::size_t>(x) * 4, 1, 3, out);
        }
        const bool written = std::fclose(out) == 0;
        if (!written)
            std::fprintf(stderr, "capture: writing %s failed\n", path.c_str());
    }

    GLFWwindow* window = nullptr;
    std::string iniFilename;
    TuiSettings* settings = nullptr;
    std::string pendingCapturePath;
    int requestedMinWidth = 0;
    int requestedMinHeight = 0;
    bool limitsRequested = false;
};

} // namespace

std::unique_ptr<Backend> makeGuiBackend(const std::string& iniFilename, TuiSettings& settings) {
    return std::make_unique<GuiBackend>(iniFilename, &settings);
}

} // namespace llmfun::tui
