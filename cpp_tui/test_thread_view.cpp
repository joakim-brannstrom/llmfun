// test_thread_view.cpp
//
// Headless unit tests for the thread-state derivation (task 8, design 4.11 /
// FR-19): which body the thread area shows when it has nothing to show, plus the
// copy and the starter-action table that body is built from.
//
// What is pinned here, case by case:
//   - the whole precedence table (all eight combinations of the three facts), so
//     a later reordering of deriveThreadView is a visible decision, not a drift:
//     loading > messages > offline > empty;
//   - the two rules the design states on its own: a session switch is a skeleton
//     even when the old thread still holds rows, and Offline wins over Empty;
//   - the third rule that keeps the two consistent: an existing transcript is
//     never hidden by an outage, so messages beat offline;
//   - the empty-state copy is the design's, and its text-grid form differs from
//     it only by the documented em-dash substitution (an em dash is East-Asian
//     ambiguous: one cell in imtui, two in a CJK terminal);
//   - three starter actions, each with a label and a prompt, and a prompt that
//     is never empty (a button that fills nothing is a no-op button);
//   - the skeleton is three rows with a 1.2 s cycle and a distinct grid glyph.
//
// Layering check, by construction: this binary compiles model/thread_view.cpp
// directly and links nothing else, so the module cannot have grown an
// ImGui/tui.h dependency.
//
// Exit codes: 0 = all assertions pass; 1 = first assertion failure.
// Run from the build dir: ./test_thread_view

#include "model/thread_view.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace model = llmfun::tui::model;

namespace {

int g_scenario = 0;
const char* g_phase = "startup";

void fail(const std::string& what) {
    std::fprintf(stderr, "FAIL [scenario %d - %s]: %s\n", g_scenario, g_phase, what.c_str());
    std::exit(1);
}

void expect(bool condition, const std::string& what) {
    if (!condition)
        fail(what);
}

void phase(const char* what) { g_phase = what; }

const char* viewName(model::ThreadView view) {
    switch (view) {
    case model::ThreadView::Ready:
        return "Ready";
    case model::ThreadView::Empty:
        return "Empty";
    case model::ThreadView::Loading:
        return "Loading";
    case model::ThreadView::Offline:
        return "Offline";
    }
    return "?";
}

/// One row of the precedence table.
void expectView(bool hasMessages, bool loadingSession, bool offline, model::ThreadView want) {
    const model::ThreadView got = model::deriveThreadView(hasMessages, loadingSession, offline);
    if (got == want)
        return;
    fail(std::string("hasMessages=") + (hasMessages ? "1" : "0") +
         " loadingSession=" + (loadingSession ? "1" : "0") + " offline=" + (offline ? "1" : "0") +
         " -> " + viewName(got) + ", expected " + viewName(want));
}

bool isAscii(const std::string& text) {
    for (unsigned char c : text)
        if (c >= 0x80)
            return false;
    return true;
}

/// Replaces every UTF-8 em dash (E2 80 94) with `dash`.
std::string withAsciiDash(const std::string& text, char dash = '-') {
    const std::string emDash = "\xE2\x80\x94";
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text.compare(i, emDash.size(), emDash) == 0) {
            out.push_back(dash);
            i += emDash.size();
            continue;
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

// --- 1: the precedence table ------------------------------------------------

void scenarioPrecedence() {
    g_scenario = 1;
    phase("no messages, nothing pending -> Empty");
    expectView(false, false, false, model::ThreadView::Empty);

    phase("loading wins over empty");
    expectView(false, true, false, model::ThreadView::Loading);

    phase("offline wins over empty");
    expectView(false, false, true, model::ThreadView::Offline);

    phase("offline wins over empty while a session loads too? loading is checked first");
    expectView(false, true, true, model::ThreadView::Loading);

    phase("messages -> Ready");
    expectView(true, false, false, model::ThreadView::Ready);

    phase("a session switch replaces the thread: loading wins over messages");
    expectView(true, true, false, model::ThreadView::Loading);
    expectView(true, true, true, model::ThreadView::Loading);

    phase("an existing transcript is never hidden by an outage: messages win over offline");
    expectView(true, false, true, model::ThreadView::Ready);
}

// --- 2: the empty-state copy ------------------------------------------------

void scenarioEmptyCopy() {
    g_scenario = 2;
    phase("wordmark");
    expect(std::string(model::kEmptyWordmark) == "llmfun",
           std::string("wordmark is '") + model::kEmptyWordmark + "', expected 'llmfun'");

    phase("sentence is the design's, verbatim");
    const std::string sentence(model::kEmptySentence);
    const std::string wanted = "Ask, plan, or code \u2014 the agent can read your workarea, search "
                               "the RAG, and run tools.";
    expect(sentence == wanted, "sentence '" + sentence + "' != '" + wanted + "'");
    expect(sentence != model::kEmptySentenceAscii,
           "the grid form equals the pixel form: the em dash was not substituted");

    phase("the grid form is pure ASCII");
    const std::string gridSentence(model::kEmptySentenceAscii);
    expect(isAscii(gridSentence), "grid sentence is not ASCII: '" + gridSentence + "'");

    phase("the two forms differ by the em-dash substitution and nothing else");
    expect(withAsciiDash(sentence) == gridSentence,
           "grid sentence '" + gridSentence +
               "' is not the pixel sentence with '-' for the em "
               "dash ('" +
               withAsciiDash(sentence) + "')");

    phase("palette hint");
    expect(std::string(model::kPaletteHint) == "Ctrl+K",
           std::string("hint is '") + model::kPaletteHint + "', expected 'Ctrl+K'");
}

// --- 3: the starter actions -------------------------------------------------

void scenarioStarterActions() {
    g_scenario = 3;
    phase("three actions");
    expect(model::kStarterActionCount == 3,
           "expected 3 starter actions, got " + std::to_string(model::kStarterActionCount));

    const char* wantedLabels[3] = {"Summarize this workarea", "Plan a feature",
                                   "Find failing tests"};
    for (std::size_t i = 0; i < 3; ++i) {
        const model::StarterAction& action = model::kStarterActions[i];
        if (std::string(action.label) != wantedLabels[i])
            fail("action " + std::to_string(i) + " label is '" + action.label + "', expected '" +
                 wantedLabels[i] + "'");
        if (action.prompt == nullptr || *action.prompt == '\0')
            fail("action " + std::to_string(i) + " has no prompt to fill the composer with");
        // A distinct prompt per action: three buttons that fill the same text are
        // one button drawn three times.
        for (std::size_t j = 0; j < i; ++j)
            if (std::string(model::kStarterActions[j].prompt) == action.prompt)
                fail("actions " + std::to_string(j) + " and " + std::to_string(i) +
                     " fill the composer with the same text");
    }
}

// --- 4: the loading and offline copy ---------------------------------------

void scenarioStateCopy() {
    g_scenario = 4;
    phase("skeleton");
    expect(model::kSkeletonRows == 3,
           "expected a 3-row skeleton, got " + std::to_string(model::kSkeletonRows));
    expect(model::kSkeletonShimmerSeconds == 1.2,
           "the shimmer cycle is no longer 1.2 s (a capture and a golden would drift apart)");
    expect(std::string(model::kSkeletonGlyphAscii) == "...",
           std::string("the grid skeleton glyph is '") + model::kSkeletonGlyphAscii +
               "', expected three ASCII dots");
    expect(isAscii(model::kSkeletonGlyphAscii), "the grid skeleton glyph is not ASCII");

    phase("offline card");
    expect(std::string(model::kOfflineHeadline) == "! model server unreachable",
           std::string("headline is '") + model::kOfflineHeadline +
               "', expected '! model server unreachable'");
    expect(std::string(model::kOfflineDetail).find('\n') == std::string::npos,
           "the offline explanation must stay one sentence, not a paragraph");
    expect(std::string(model::kRetryLabel) == "Retry",
           std::string("retry label is '") + model::kRetryLabel + "', expected 'Retry'");

    // Every offline string is drawn in both profiles, so all of them have to be
    // ASCII (the grid has no fallback for them).
    for (const char* text :
         {model::kOfflineHeadline, model::kOfflineDetail, model::kRetryLabel,
          model::kRetryNothingYet, model::kRetryRunActive, model::kComposerOfflineTooltip,
          model::kEmptyWordmark, model::kPaletteHint}) {
        if (!isAscii(text))
            fail(std::string("UI string is not ASCII and has no grid substitute: '") + text + "'");
    }
}

} // namespace

int main() {
    scenarioPrecedence();
    scenarioEmptyCopy();
    scenarioStarterActions();
    scenarioStateCopy();
    std::printf("OK: thread-view derivation and copy pinned (%d scenarios)\n", g_scenario);
    return 0;
}
