/// Backend seam: one lifecycle interface for the terminal (ncurses/ImTui),
/// graphical (GLFW/OpenGL3) and inert (Null) backends. Implementations are
/// created via the make*Backend factories and owned by the caller.
#pragma once

#include <memory>
#include <string>

#include "tui_settings.h"

namespace llmfun::tui {

/// Active backend kind; the v4 C API surfaces it as TuiBackendKind.
enum class BackendKind { None = 0, Tui = 1, Gui = 2 };

/// Surface geometry of a pixel-profile backend, in screen coordinates: pixels
/// at content scale 1.0, the only scale at which they equal framebuffer pixels.
///
/// The resize floor is reported twice on purpose. `limitsRequested` says the
/// backend asked the window system for `minWidth x minHeight`; `limitsApplied`
/// says the window system actually holds it. glfw has no limits getter, so the
/// second one is read back from the window system and `limitsObserved` says
/// whether this platform can answer at all - `limitsApplied == false` with
/// `limitsObserved == false` means "not verifiable here", not "not set".
/// 0/false for the min pair = no floor was asked for.
struct GuiWindowGeometry {
    int width = 0;
    int height = 0;
    int minWidth = 0;
    int minHeight = 0;
    bool limitsRequested = false;
    bool limitsApplied = false;
    bool limitsObserved = false;
};

struct Backend {
    virtual ~Backend() = default;

    /// Attach resources. false = failure; `error` receives the precise reason.
    virtual bool init(std::string& error) = 0;

    /// Pump input and begin the frame (ImGui::NewFrame() last).
    virtual void newFrame() = 0;

    /// Present the finished frame.
    virtual void renderFrame() = 0;

    /// Release everything owned; called exactly once after a successful init.
    /// A failed init must unwind internally (the backend is then dropped
    /// without shutdown()).
    virtual void shutdown() = 0;

    virtual BackendKind kind() const = 0;

    /// true once the backend observed an exit request (window close; Ctrl+C stays core-side).
    virtual bool shouldClose() const = 0;

    /// Write the NEXT rendered frame to `path` as a binary PPM (P6, 8 bit per
    /// channel). GUI only: the pixel profile is the only backend that has pixels
    /// to read back, so the default implementation refuses the request. Returns
    /// false when the request cannot be honoured (no window, unsupported
    /// backend); a readback that fails at render time is reported on stderr by
    /// the backend.
    ///
    /// The capture happens inside renderFrame(), between drawing and the buffer
    /// swap, because that is the only point where the finished frame is still
    /// the back buffer. Task 19's golden harness builds on this seam.
    virtual bool captureNextFramePpm(const std::string& path) {
        (void)path;
        return false;
    }

    /// Geometry the backend applied to its surface: the live window size in
    /// screen coordinates plus the resize floor it asked the window system for
    /// and whether the window system holds it. GUI only, like the capture above -
    /// a cell grid has no pixel window - so the default refuses. The
    /// window-defaults test (task 9) and the golden harness (task 19) read the
    /// surface they are looking at through this.
    virtual bool guiWindowGeometry(GuiWindowGeometry& geometry) const {
        (void)geometry;
        return false;
    }
};

/// A sink for the window title: what a pixel backend installs when it creates its window.
/// GLFW copies the string, so the reference lives for the call only.
using WindowTitleSink = void (*)(const std::string& title);

/// The application's own name ("llmfun"): what the window is called before any session exists
/// and what an empty title falls back to. One definition, because a window that opens with one
/// name and restores another after a cleared title is two facts pretending to be one.
const char* applicationWindowTitle();

/// Install the sink `tuiBackendSetWindowTitle()` forwards to (`nullptr` = none, which makes
/// the seam a no-op again). One process has one window, so one slot; installing a sink
/// also forgets the last forwarded title, so a backend that calls this *after* something
/// asked for a title still receives the current one.
void setWindowTitleSink(WindowTitleSink sink);

/// Mirror a title into the OS window title (FR-02, P-Δ2).
///
/// Internal C++ on purpose: §5.2's v5 ABI list has no title call, and the only caller is the
/// workbench frame. `""` means the application name ("llmfun"), so a state with no session
/// undoes a mirror without the caller knowing the name.
///
/// It is a no-op where nothing can show a title - the cell grid, a headless harness, a state
/// without a backend - because it forwards to a *sink* and answers nothing when none is
/// installed. That is what makes the call legal from the shared frame path, and it is also
/// why the comparison lives here: a title that did not change forwards nothing, so a frame
/// loop calling this every frame costs one string compare (never a window-system call).
void tuiBackendSetWindowTitle(const std::string& title);

/// How many titles the seam has forwarded (a change it accepted, with or without a sink).
/// Harness seam, not app surface: the counter is the only way to see "once per change, not once
/// per frame" from outside, which is what `./test_topbar` asserts.
unsigned windowTitleChangeCount();

/// The last title the seam forwarded; "" before the first change. Harness seam, same reason:
/// a headless run has no window to read the title back from.
const std::string& lastWindowTitle();

std::unique_ptr<Backend> makeNcursesBackend();
std::unique_ptr<Backend> makeNullBackend();
/// `iniFilename` is the imgui settings file (io.IniFilename); `settings` is the
/// non-owning view-settings record: loaded from settingsPath(iniFilename)
/// before the window, saved (with the final window size) at shutdown. Empty
/// iniFilename disables persistence.
std::unique_ptr<Backend> makeGuiBackend(const std::string& iniFilename, TuiSettings& settings);

} // namespace llmfun::tui
