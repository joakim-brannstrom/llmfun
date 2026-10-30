/// The internal window-title seam (FR-02, P-Δ2): the top bar mirrors the session title into
/// the OS window title, and nothing crosses the C ABI to do it.
///
/// The seam owns the two things a caller should not have to remember: `""` means the
/// application name, and a title that has not changed is not forwarded again (so the frame
/// loop may call it unconditionally - one string compare per frame, never a window-system
/// call).
///
/// It forwards to a *sink* that a pixel backend installs when its window exists and clears
/// when it is destroyed, so the cell grid, a headless harness and a state without a backend
/// are all the same no-op. That is what makes the call legal on the shared frame path, and
/// it is why the seam has its own translation unit: it is backend-agnostic (it knows only
/// the function pointer), while the GUI file is the one that knows a GLFW window.
#include "tui_backend.h"

#include <string>
#include <string_view>

namespace llmfun::tui {
namespace {

/// What the window is called before a session exists and what an empty title falls back to.
/// One definition for the whole tree (`applicationWindowTitle()`): the pixel backend opens its
/// window with the same string this fallback restores.
constexpr const char* kAppName = "llmfun";

WindowTitleSink g_sink = nullptr;

/// The title the sink was last given - not "the last value asked for": a backend that
/// installs its sink later must still receive the current title, which is why
/// `setWindowTitleSink()` clears this.
std::string g_forwarded;

unsigned g_changes = 0;

} // namespace

const char* applicationWindowTitle() { return kAppName; }

void setWindowTitleSink(WindowTitleSink sink) {
    g_sink = sink;
    g_forwarded.clear();
}

void tuiBackendSetWindowTitle(const std::string& title) {
    // A view rather than a temporary string: the fallback path must not allocate on a frame
    // that changes nothing.
    const std::string_view wanted =
        title.empty() ? std::string_view(applicationWindowTitle()) : std::string_view(title);
    if (wanted == std::string_view(g_forwarded))
        return;
    g_forwarded.assign(wanted);
    ++g_changes;
    if (g_sink != nullptr)
        g_sink(g_forwarded);
}

unsigned windowTitleChangeCount() { return g_changes; }

const std::string& lastWindowTitle() { return g_forwarded; }

} // namespace llmfun::tui
