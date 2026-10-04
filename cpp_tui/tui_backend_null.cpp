/// Inert Null backend behind the Backend seam.
/// Attaches no resources and renders nothing; kinds out as None so callers can
/// keep a state backend-less (headless/tests) without null-checks.
#include "tui_backend.h"

namespace llmfun::tui {

namespace {

class NullBackend final : public Backend {
public:
    bool init(std::string&) override { return true; }

    void newFrame() override {}

    void renderFrame() override {}

    void shutdown() override {}

    BackendKind kind() const override { return BackendKind::None; }

    bool shouldClose() const override { return false; }
};

} // namespace

std::unique_ptr<Backend> makeNullBackend() { return std::make_unique<NullBackend>(); }

} // namespace llmfun::tui
