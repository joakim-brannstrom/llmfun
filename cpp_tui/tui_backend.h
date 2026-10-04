/// Backend seam: one lifecycle interface for the terminal (ncurses/ImTui),
/// graphical (GLFW/OpenGL3) and inert (Null) backends. Implementations are
/// created via the make*Backend factories and owned by the caller.
#pragma once

#include <memory>
#include <string>

namespace llmfun::tui {

/// Active backend kind; the v4 C API surfaces it as TuiBackendKind.
enum class BackendKind { None = 0, Tui = 1, Gui = 2 };

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
};

std::unique_ptr<Backend> makeNcursesBackend();
std::unique_ptr<Backend> makeNullBackend();
std::unique_ptr<Backend> makeGuiBackend();

} // namespace llmfun::tui
