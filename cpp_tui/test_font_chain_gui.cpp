// test_font_chain_gui.cpp - pins the configured font chain (tui.fonts) end to end on
// a live GUI atlas: makeGuiBackend() -> configureUiFont() on a real backend in a
// real window, every case asserting by glyph lookup on the live `ImFont`s plus
// the stderr log, never by pixel diffing. Test fonts: in-repo TTFs (Karla primary,
// Cousine tail, embedded Roboto floor). Headless: xvfb-run -s "-screen 0 1280x820x24"
// ./test_font_chain_gui

#include "tui.h"
#include "tui_backend.h"

#include "imtui/imtui.h" // imgui core (the build's IMGUI_USE_WCHAR32 applies)

#include "imgui_rich_md/rich_md_host.h" // ReadAssetDefault: the asset-table probe
#include "ui/tokens.h"                  // ui::codeFont/setCodeFont, scaledTypeSize, uiFontFace

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h> // dup/dup2/close/read: the fd-2 capture

namespace {

constexpr int kTestFontStrategy = 1; // 1 = in-repo asset (the dev image installs no system fonts)

/// The in-repo faces, and the codepoints the coverage difference gives the tests.
const std::string kFontDir = TEST_FONT_CHAIN_FONT_DIR;
const std::string kPrimaryFont = kFontDir + "/Karla-Regular.ttf";
const std::string kTailFont = kFontDir + "/Cousine-Regular.ttf";
const std::string kMissingFont = kFontDir + "/no-such-font.ttf";
// U+2215 DIVISION SLASH: only the file Cousine carries it among the faces
// involved.
constexpr ImWchar kTailOnlyCp = static_cast<ImWchar>(0x2215);
// U+0132 LATIN CAPITAL LIGATURE IJ: only the embedded Roboto-Medium carries it
// among the faces the configured chain involves.
constexpr ImWchar kFloorCp = static_cast<ImWchar>(0x0132);
// 'A': every face involved carries it (the "any live atlas serves this" control).
constexpr ImWchar kEverywhereCp = static_cast<ImWchar>(0x0041);

/// The probe range the glyph-coverage snapshots cover: the BMP block the three
/// roles' differences live in (nothing here needs beyond it).
constexpr unsigned kProbeFirst = 0x20;
constexpr unsigned kProbeLast = 0x3000; // exclusive

int g_failures = 0;
const char* g_phase = "startup";

/// Setup problem: the remaining checks would only report the consequence.
[[noreturn]] void fail(const std::string& what) {
    std::fprintf(stderr, "SETUP [%s]: %s\n", g_phase, what.c_str());
    std::exit(2);
}

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok)
        ++g_failures;
}

/// stderr capture: redirect fd 2 into a pipe around the backend init and read
/// the captured text back (the test_rich_markdown_text ErrCapture pattern).
/// init() prints every line the chain owns - the ui: window line, the per-step
/// font lines and the one type= line - so one capture per case sees them all.
struct ErrCapture {
    int saved = -1;
    int pipeR = -1;
    bool active = false;

    void start() {
        ::fflush(stderr);
        int fds[2] = {-1, -1};
        saved = ::dup(2);
        const bool piped = ::pipe(fds) == 0;
        if (saved < 0 || !piped || ::dup2(fds[1], 2) != 2) {
            if (saved >= 0)
                ::close(saved);
            if (piped) {
                ::close(fds[0]);
                ::close(fds[1]);
            }
            return;
        }
        ::close(fds[1]);
        pipeR = fds[0];
        active = true;
    }

    void stop(std::string& out) {
        if (!active)
            return;
        active = false;
        ::fflush(stderr); // stderr is unbuffered, but do not rely on it
        ::close(2);
        // One read loop to true EOF: this process is single-threaded and the
        // write end of the pipe was closed above (the init-time capture owns
        // fd 2 for the whole init), so once read() returns 0 there is no
        // writer left - any "late" log line from the failing add has already
        // landed in the pipe before this point, never after.
        std::string s;
        char buf[4096];
        ssize_t n;
        while ((n = ::read(pipeR, buf, sizeof(buf))) > 0)
            s.append(buf, static_cast<std::size_t>(n));
        ::close(pipeR);
        pipeR = -1;
        ::dup2(saved, 2);
        ::close(saved);
        saved = -1;
        out.swap(s);
    }
};

/// One glyph-coverage snapshot of a face: the probe range, one bit per
/// codepoint. The primary-fallback vs baseline identity compares these.
std::vector<bool> coverageSnapshot(ImFont* font) {
    std::vector<bool> covered(kProbeLast - kProbeFirst, false);
    if (font == nullptr)
        return covered;
    for (unsigned cp = kProbeFirst; cp < kProbeLast; ++cp)
        covered[cp - kProbeFirst] = font->IsGlyphInFont(static_cast<ImWchar>(cp));
    return covered;
}

/// What one case needs: the configured chain (empty = the builtin baseline).
struct FontCase {
    const char* name;
    std::vector<std::string> chain;
};

/// What one case produced while its atlas was live.
struct CaseResult {
    std::string log;
    int fontCount = 0;
    std::vector<bool> defaultCoverage;
    std::vector<bool> codeCoverage;
};

/// The ordered-substring matcher: every needle must appear, in order, after the
/// previous one. A missing or out-of-order line fails with both names.
void checkLogOrder(const std::string& log, const std::vector<std::string>& lines,
                   const std::string& label) {
    std::size_t cursor = 0;
    for (const std::string& line : lines) {
        const std::size_t at = log.find(line, cursor);
        check(at != std::string::npos,
              label + ": stderr carries, in order, \"" + line + "\"" +
                  (at == std::string::npos ? " (missing or out of order)" : ""));
        if (at == std::string::npos)
            return; // the remaining lines would only report the consequence
        cursor = at + line.size();
    }
}

/// The one line per tail entry that CAN be there: the embedded merge logs
/// nothing when the build has no asset table (the carried task-4 note M1). The
/// probe mirrors configureUiFont's own condition (both read the same table), so
/// the test never asserts a line the build under test cannot print.
bool haveEmbeddedAssets() {
    return RichMd::ReadAssetDefault(llmfun::tui::ui::uiFontFace().asset).has_value();
}

/// Runs one case end to end and asserts everything the case owns: a fresh GUI backend with
/// the case's chain, the init stderr captured, the live-atlas lookups and the atlas font
/// count the chain left (read BEFORE the two settle frames - the markdown host's own faces
/// must never leak into a chain-count claim). Setup mirrors test_gui_window's (makeGuiBackend
/// + init + initMarkdownGuiMode + applyUiTheme, no loadUiConf); `baseline` is case 7's result,
/// needed only by case 6 for the no-residue identity; null otherwise.
CaseResult runCase(const FontCase& c, const CaseResult* baseline) {
    g_phase = c.name;
    const std::string label = c.name;

    // A clean atlas per case: the previous case's fonts must not survive into
    // this one's font-count and coverage assertions (the atlas belongs to the
    // per-process context; Clear() is the documented between-frames reset and
    // the previous backend was shut down first).
    llmfun::tui::ui::setCodeFont(nullptr);
    ImGui::GetIO().Fonts->Clear();
    ImGui::GetIO().FontDefault = nullptr;
    ImGui::GetStyle() = ImGuiStyle();

    llmfun::tui::TuiState state;
    std::string error;
    auto backend = llmfun::tui::makeGuiBackend(state.ui.prefs, c.chain);

    ErrCapture capture;
    capture.start();
    const bool inited = backend->init(error);
    CaseResult r;
    capture.stop(r.log);
    if (!inited)
        fail("GUI backend init failed (no display?): " + error);
    state.backend = std::move(backend);
    llmfun::tui::initMarkdownGuiMode(state);
    llmfun::tui::applyUiTheme();

    // The atlas as the chain left it, and the two faces' coverage.
    r.fontCount = static_cast<int>(ImGui::GetIO().Fonts->Fonts.size());
    r.defaultCoverage = coverageSnapshot(ImGui::GetIO().FontDefault);
    r.codeCoverage = coverageSnapshot(llmfun::tui::ui::codeFont());

    // The shared live-atlas facts: the atlas is not empty (the pin stays
    // terminal), both faces are set, and the type scale still owns the drawn
    // base - a font file's own metrics cannot move it (invariant 6).
    check(r.fontCount > 0, label + ": the atlas is not empty (the pin stays terminal)");
    check(ImGui::GetIO().FontDefault != nullptr, label + ": io.FontDefault is set");
    check(llmfun::tui::ui::codeFont() != nullptr, label + ": the code face is set");
    const float wantBase = llmfun::tui::ui::scaledTypeSize(llmfun::tui::ui::TypeStep::body, 0.0f);
    check(ImGui::GetStyle().FontSizeBase == wantBase,
          label + ": the drawn base stays the body step (" +
              std::to_string(ImGui::GetStyle().FontSizeBase) + ", expected " +
              std::to_string(wantBase) + ")");

    // The rendered-glyph smoke (the optional complement to the lookups): the
    // chain survives real frames - two settle frames draw the shell through the
    // merged faces, which no count or lookup alone proves.
    if (!c.chain.empty()) {
        const auto frames = [&state](int count) {
            for (int i = 0; i < count; ++i) {
                state.backend->newFrame();
                if (!llmfun::tui::tuiRender(state))
                    fail("tuiRender asked to quit");
                state.backend->renderFrame();
            }
        };
        frames(2);

        check(ImGui::GetStyle().FontSizeBase == wantBase,
              label + ": the drawn base stays the body step through real frames");
    }

    if (c.name == std::string{"chain-semantics"}) {
        // ---- case 1: chain semantics ----
        // The stderr order IS the chain: the primary logs first, the tail's
        // merge follows, the embedded Roboto merges LAST, and the type line
        // (one per init, never per font) closes the capture. The exact
        // per-step formats are the task-4 contract.
        std::vector<std::string> lines = {
            "ui: font=" + kPrimaryFont + " (configured primary)",
            "ui: font=" + kTailFont + " (merged)",
        };
        if (haveEmbeddedAssets())
            lines.push_back("ui: font=Roboto-Medium (embedded, merged)");
        lines.push_back("ui: type=15 px base (body step)");
        checkLogOrder(r.log, lines, label);
        // No builtin-chain lines on a configured run: the builtin faces never
        // become faces of their own there.
        check(r.log.find("(embedded)\n") == std::string::npos &&
                  r.log.find("(embedded code)\n") == std::string::npos,
              label + ": the builtin faces are not faces of their own");
        // The family gate: the primary's family is what FontDefault serves.
        check(std::string(ImGui::GetIO().FontDefault->GetDebugName()) == "Karla-Regular.ttf",
              label + ": FontDefault's family is the configured primary (" +
                  std::string(ImGui::GetIO().FontDefault->GetDebugName()) + ")");
        // A merge never becomes a face of its own: the chain's ONE font (three
        // sources merged into it: Karla + Cousine + embedded Roboto), plus the
        // five faces the markdown host itself adds (the Roboto emphasis
        // variants and its code face) - six fonts at init in total.
        check(r.fontCount == 6, label + ": one chain font + five markdown faces (" +
                                    std::to_string(r.fontCount) + ")");
    } else if (c.name == std::string{"code-face-identity"}) {
        // ---- case 2: the code face IS the primary (invariant 3) ----
        check(llmfun::tui::ui::codeFont() == ImGui::GetIO().FontDefault,
              label + ": ui::codeFont() IS io.FontDefault (pointer equality)");
        ImFont* primary = ImGui::GetIO().FontDefault;
        // The embedded Roboto merge is the optional one (the carried task-4 M1):
        // without the asset table the primary carries only the two configured
        // sources, and the message says so.
        const int wantSources = haveEmbeddedAssets() ? 3 : 2;
        check(primary->Sources.size() == wantSources,
              label + ": the primary carries " + std::to_string(wantSources) + " merged sources (" +
                  std::to_string(primary->Sources.size()) + ", Karla + Cousine" +
                  std::string(haveEmbeddedAssets() ? " + embedded Roboto" : "") + ")");
        // And the tail-only glyph rides the SAME instance: a separately loaded
        // copy of Karla alone would answer no here.
        check(primary->IsGlyphInFont(kTailOnlyCp),
              label + ": the merged instance serves the tail's glyph");
    } else if (c.name == std::string{"tail-fallback"}) {
        // ---- case 3: the fallback resolves on both faces ----
        check(ImGui::GetIO().FontDefault->IsGlyphInFont(kTailOnlyCp),
              label + ": the tail-only codepoint resolves on FontDefault (no tofu)");
        check(llmfun::tui::ui::codeFont()->IsGlyphInFont(kTailOnlyCp),
              label + ": ...and on the code face (code spans inherit the chain)");
    } else if (c.name == std::string{"roboto-floor"}) {
        // ---- case 4: the Roboto floor resolves on both faces ----
        check(ImGui::GetIO().FontDefault->IsGlyphInFont(kFloorCp),
              label + ": the codepoint no configured font carries resolves on FontDefault");
        check(llmfun::tui::ui::codeFont()->IsGlyphInFont(kFloorCp),
              label + ": ...and on the code face (the embedded merge is the floor)");
        // The control: a face-served codepoint too, so the assertions above can
        // never pass on an atlas that resolved nothing at all.
        check(ImGui::GetIO().FontDefault->IsGlyphInFont(kEverywhereCp),
              label + ": the control codepoint resolves (the atlas is live)");
    } else if (c.name == std::string{"tail-skip"}) {
        // ---- case 5: the missing tail is skipped, no residue ----
        // The primary loaded, the tail was skipped with the convention, and the
        // embedded merge still happened - in that order.
        std::vector<std::string> lines = {
            "ui: font=" + kPrimaryFont + " (configured primary)",
            "ui: font=" + kMissingFont + " cannot load; skipped",
        };
        if (haveEmbeddedAssets())
            lines.push_back("ui: font=Roboto-Medium (embedded, merged)");
        lines.push_back("ui: type=15 px base (body step)");
        checkLogOrder(r.log, lines, label);
        // The skip is silent in the atlas: no "(merged)" line for the tail and
        // exactly one font (the primary) carrying exactly two merged sources
        // (Karla + embedded Roboto; the failed add registered nothing).
        check(r.log.find("(merged)") == std::string::npos,
              label + ": the skipped tail logged no (merged) line");
        check(r.fontCount == 6,
              label +
                  ": one chain font (the skipped tail registered nothing) + five markdown faces (" +
                  std::to_string(r.fontCount) + ")");
        check(r.defaultCoverage[kFloorCp - kProbeFirst],
              label + ": the embedded floor still resolves (the merge happened)");
        check(!r.defaultCoverage[kTailOnlyCp - kProbeFirst],
              label + ": the skipped tail's glyph is gone (the skip is real)");
    } else if (c.name == std::string{"primary-fallback"}) {
        // ---- case 6: broken fonts[0] -> the FULL builtin chain ----
        // The carried task-4 note I2 / task-5 Info-1: the primary's failure is
        // silent - pin the ABSENCE of the configured-primary line deliberately.
        check(r.log.find("(configured primary)") == std::string::npos,
              label + ": no configured-primary line (the failure is silent by design)");
        check(r.log.find("(merged)") == std::string::npos,
              label + ": no (merged) line (the tail loop never ran)");
        // What IS there: the builtin chain, in its golden order.
        std::vector<std::string> lines;
        if (haveEmbeddedAssets()) {
            lines.push_back("ui: font=Roboto-Medium (embedded)");
            lines.push_back("ui: font=Inconsolata-Medium (embedded code)");
        }
        lines.push_back("ui: type=15 px base (body step)");
        checkLogOrder(r.log, lines, label);
        // No residue in the atlas: the font count is the no-config run's, and
        // the coverage is too (the snapshot identity, below while both are
        // comparable... they are: the baseline ran before this case).
        check(baseline != nullptr && r.fontCount == baseline->fontCount,
              label + ": the atlas font count is the no-config run's (" +
                  std::to_string(r.fontCount) + " vs " +
                  std::to_string(baseline ? baseline->fontCount : -1) + ")");
        check(baseline != nullptr && r.defaultCoverage == baseline->defaultCoverage,
              label + ": FontDefault's glyph coverage equals the no-config run's");
        check(baseline != nullptr && r.codeCoverage == baseline->codeCoverage,
              label + ": the code face's glyph coverage equals the no-config run's");
    } else if (c.name == std::string{"empty-baseline"}) {
        // ---- case 7: the empty chain is the builtin chain ----
        // The expected stderr order is the no-config golden: the chain is
        // exactly the builtin chain.
        std::vector<std::string> lines;
        if (haveEmbeddedAssets()) {
            lines.push_back("ui: font=Roboto-Medium (embedded)");
            lines.push_back("ui: font=Inconsolata-Medium (embedded code)");
        }
        lines.push_back("ui: type=15 px base (body step)");
        checkLogOrder(r.log, lines, label);
        check(r.log.find("(configured primary)") == std::string::npos &&
                  r.log.find("(merged)") == std::string::npos &&
                  r.log.find("cannot load") == std::string::npos,
              label + ": no configured-chain lines on the baseline");
        // The base size is the type scale's body step (15 px) - the collapse
        // test_gui_window's expectedFontSizeBase() pins.
        check(wantBase == 15.0f,
              label + ": the body step is 15 px (" + std::to_string(wantBase) + ")");
        // Two builtin chain fonts + the five the markdown host adds.
        check(r.fontCount == 7, label + ": two builtin chain fonts + five markdown faces (" +
                                    std::to_string(r.fontCount) + ")");
        // And the two faces are distinct families on the builtin chain (the
        // code face is Inconsolata there, not the merged primary) - the shape
        // case 2 asserts the negation of.
        check(llmfun::tui::ui::codeFont() != ImGui::GetIO().FontDefault,
              label + ": the code face is a separate family on the builtin chain");
    } else {
        fail("unhandled case " + label);
    }

    // The teardown: markdown textures go while the backend is still alive, then
    // the backend shuts down and the atlas is left to the next case's Clear().
    llmfun::tui::destroyMarkdown(state);
    state.backend->shutdown();
    state.backend.reset();
    return r;
}

} // namespace

int main() {
    std::setlocale(LC_ALL, "");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;

    // Cases 1-5 in listed order, then the baseline (7), then case 6 - which
    // runs last because its no-residue identity compares against the baseline's
    // atlas. The listed order is unchanged; only the execution puts 7 before 6.
    const FontCase cases[] = {
        // 1. Chain semantics: primary, merged tail, embedded last.
        {"chain-semantics", {kPrimaryFont, kTailFont}},
        // 2. Code-face identity (the same chain; the pointer is the claim).
        {"code-face-identity", {kPrimaryFont, kTailFont}},
        // 3. The tail-only glyph resolves (the same chain; the lookup is the claim).
        {"tail-fallback", {kPrimaryFont, kTailFont}},
        // 4. The Roboto-floor glyph resolves (the same chain; the lookup is the claim).
        {"roboto-floor", {kPrimaryFont, kTailFont}},
        // 5. A missing tail is skipped; the primary and the embedded merge survive.
        {"tail-skip", {kPrimaryFont, kMissingFont}},
        // 7. The empty chain is the builtin chain (the no-config baseline).
        {"empty-baseline", {}},
        // 6. A broken fonts[0] falls back to the FULL builtin chain.
        {"primary-fallback", {kMissingFont, kPrimaryFont}},
    };
    constexpr int kBaselineIndex = 5; // empty-baseline, run before primary-fallback

    runCase(cases[0], nullptr);
    runCase(cases[1], nullptr);
    runCase(cases[2], nullptr);
    runCase(cases[3], nullptr);
    runCase(cases[4], nullptr);
    // The baseline (case 7) runs before case 6 and its result is what case 6's
    // no-residue identity compares against.
    const CaseResult baseline = runCase(cases[kBaselineIndex], nullptr);
    runCase(cases[kBaselineIndex + 1], &baseline);
    ImGui::DestroyContext();
    if (g_failures == 0) {
        std::printf("all checks passed\n");
        return 0;
    }
    std::printf("%d check(s) FAILED\n", g_failures);
    return 1;
}
