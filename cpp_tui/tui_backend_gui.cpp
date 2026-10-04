/// GUI backend behind the Backend seam: GLFW window + OpenGL3 renderer.
/// ImTui is bypassed entirely in GUI mode: stock Dear ImGui renders through
/// the vendored GLFW platform backend and OpenGL3 renderer backend.
#include "tui_backend.h"

#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <cstdio>
#include <cstdlib>
#include <string>

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

constexpr int WindowWidth = 800;
constexpr int WindowHeight = 600;
// audit: window title pinned to "llmfun"; verified on screen via the X11
// window list.
constexpr const char* WindowTitle = "llmfun";
// Idle pacing: wait for input or up to one 60 Hz period — never busy-poll.
constexpr double EventWaitSeconds = 1.0 / 60.0;
// Base size of the embedded pixel font (ProggyClean) used when no file font
// is configured; the pin keeps the pixel profile stable.
constexpr float EmbeddedFontSize = 13.0f;
// Default pixel size for a file font configured through LLMFUN_GUI_FONT.
constexpr float DefaultFileFontSize = 16.0f;

// LLMFUN_GUI_FONT_SIZE: pixel size for the file font. Unset, empty,
// non-numeric, or out-of-range values keep the default.
float configuredFileFontSize() {
    const char* env = std::getenv("LLMFUN_GUI_FONT_SIZE");
    if (env == nullptr || env[0] == '\0')
        return DefaultFileFontSize;
    char* end = nullptr;
    const float px = std::strtof(env, &end);
    if (end == env || *end != '\0' || !(px > 0.0f) || px > 512.0f)
        return DefaultFileFontSize;
    return px;
}

// Fonts: LLMFUN_GUI_FONT (a TTF path) replaces the embedded font, its size
// coming from LLMFUN_GUI_FONT_SIZE. Without the env var the pixel profile pin
// is kept: the embedded bitmap font (ProggyClean 13 px, no asset), added
// explicitly so the choice cannot drift with AddFontDefault()'s size
// heuristic (which picks the scalable vector font once the resolved size
// reaches 15 px, e.g. under a DPI scale). The 1.92 atlas rasterizes glyphs on
// demand, so no glyph ranges are passed (they only matter to legacy
// backends); a missing file is reported and falls back to the pin instead of
// asserting (ImFontFlags_NoLoadError).
void configureFonts() {
    ImGuiIO& io = ImGui::GetIO();
    const char* path = std::getenv("LLMFUN_GUI_FONT");
    if (path != nullptr && path[0] != '\0') {
        const float sizePx = configuredFileFontSize();
        ImFontConfig cfg;
        cfg.Flags |= ImFontFlags_NoLoadError;
        if (io.Fonts->AddFontFromFileTTF(path, sizePx, &cfg) != nullptr) {
            ImGui::GetStyle().FontSizeBase = sizePx;
            return;
        }
        std::fprintf(stderr, "LLMFUN_GUI_FONT: cannot load '%s'; using the embedded font\n", path);
    }
    io.Fonts->AddFontDefaultBitmap();
    ImGui::GetStyle().FontSizeBase = EmbeddedFontSize;
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
    bool init(std::string& error) override {
        glfwLastError.clear();
        glfwSetErrorCallback(glfwErrorCallback);

        if (!glfwInit()) {
            error = "glfwInit failed: " + lastGlfwErrorText();
            return false;
        }

        // attempt 1: GL 3.0 + GLSL 130; retry once: GL 2.1 + GLSL 120.
        // Each attempt passes its OWN pinned shader string — never nullptr.
        int attempt = 0;
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
        window = glfwCreateWindow(WindowWidth, WindowHeight, WindowTitle, nullptr, nullptr);
        if (window != nullptr) {
            attempt = 1;
        } else {
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
            window = glfwCreateWindow(WindowWidth, WindowHeight, WindowTitle, nullptr, nullptr);
            if (window != nullptr)
                attempt = 2;
        }
        if (window == nullptr) {
            error = "GLFW window creation failed (GL 3.0 and GL 2.1): " + lastGlfwErrorText();
            glfwTerminate();
            return false;
        }

        glfwMakeContextCurrent(window);
        glfwSwapInterval(1);

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

        // Font selection (LLMFUN_GUI_FONT / _SIZE) and the one-time DPI
        // adaptation of the style, before the first frame.
        configureFonts();
        applyDpiScale();

        return true;
    }

    void newFrame() override {
        // Event-waiting idle: block until input arrives or one period elapses
        // (vsync stays on), so a static window never busy-spins.
        glfwWaitEventsTimeout(EventWaitSeconds);
        ImGui_ImplGlfw_NewFrame();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
    }

    void renderFrame() override {
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    void shutdown() override {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        glfwDestroyWindow(window);
        window = nullptr;
        glfwTerminate();
    }

    BackendKind kind() const override { return BackendKind::Gui; }

    bool shouldClose() const override { return window != nullptr && glfwWindowShouldClose(window); }

private:
    GLFWwindow* window = nullptr;
};

} // namespace

std::unique_ptr<Backend> makeGuiBackend() { return std::make_unique<GuiBackend>(); }

} // namespace llmfun::tui
