/// Inert Null backend behind the Backend seam.
/// Attaches no resources and renders nothing; kinds out as None so callers can
/// keep a state backend-less (headless/tests) without null-checks.
#include "tui_backend.h"

#include <chrono>
#include <thread>

namespace llmfun::tui {

namespace {

class NullBackend final : public Backend {
public:
    // No window, so no window-title sink: `tuiBackendSetWindowTitle()` (tui_backend.h) forwards
    // to whatever sink a pixel backend installed, and with none installed it is the no-op this
    // backend's title is. That is the whole "no-op on TUI/None" contract - there is nothing to
    // implement here, which is why this file did not grow a function for it.
    bool init(std::string&) override { return true; }

    void newFrame() override {}

    void renderFrame() override {}

    void shutdown() override {}

    BackendKind kind() const override { return BackendKind::None; }

    bool shouldClose() const override { return false; }

    // Sleep the timeout out: a null backend has no input source, so nothing
    // can ever wake the wait early - returning immediately would make a
    // caller's render loop spin at full CPU; the inert guards only catch a
    // backend-LESS state, and a state with THIS backend attached passes
    // them. With no events to pump and nothing drawn, sleeping is the whole
    // park. The timeout is always positive in the render path (one
    // FramePace).
    void waitEvents(std::chrono::milliseconds timeout) override {
        if (timeout > std::chrono::milliseconds::zero())
            std::this_thread::sleep_for(timeout);
    }
};

} // namespace

std::unique_ptr<Backend> makeNullBackend() { return std::make_unique<NullBackend>(); }

} // namespace llmfun::tui
