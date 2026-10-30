// test_status_chips.cpp
//
// Headless unit tests for the status-chip splitter (task 6, design §6.3 / FR-12):
// D's one-line status sentence — `formatStatusText()`, source/llm/app_agent/ui.d:421-425 —
// becomes the four chips the status row shows, until task 39 sends the same facts
// as numbers.
//
// What is pinned here, case by case:
//   - the documented sentence maps to the documented chips (`42k/128k`, `38.1 tok/s`,
//     `qwen3-coder`, `Ready`) and `Busy` is a state like `Ready`;
//   - a field the sentence does not carry renders "—", never "0" (a number and its
//     absence must stay distinguishable — the composer's mini meter shows the same
//     numbers, task 27);
//   - a malformed fragment becomes the state chip verbatim, in source order, so a
//     sentence the splitter does not fully understand loses nothing and invents nothing;
//   - an empty sentence is four "—" chips, and an empty chip is never produced;
//   - a quoted model name may contain the field separator (`| ` is only a separator
//     between fields, not inside a quoted value);
//   - the startup default sentence yields exactly `0/0`, `—`, `none`, `Ready`.
//
// Layering check, by construction: this binary compiles model/status_chips.cpp directly
// and links nothing else, so the module cannot have grown an ImGui/tui.h dependency.
//
// Exit codes: 0 = all scenarios pass; 1 = first assertion failure.
// Run from the build dir: ./test_status_chips

#include "model/status_chips.h"

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace model = llmfun::tui::model;

namespace {

int g_scenario = 0;
const char* g_phase = "startup";

void fail(const std::string& what) {
    std::fprintf(stderr, "FAIL [scenario %d — %s]: %s\n", g_scenario, g_phase, what.c_str());
    std::exit(1);
}

void expect(bool condition, const std::string& what) {
    if (!condition)
        fail(what);
}

void phase(const char* what) { g_phase = what; }

std::string quoted(const std::string& value) { return "'" + value + "'"; }

/// Asserts all four chips of one sentence.
void expectChips(const std::string& sentence, const std::string& context, const std::string& speed,
                 const std::string& model_, const std::string& state) {
    const model::StatusChips chips = model::splitStatusText(sentence);
    const std::string where = "sentence " + quoted(sentence);
    if (chips.context != context)
        fail(where + ": context " + quoted(chips.context) + " != " + quoted(context));
    if (chips.speed != speed)
        fail(where + ": speed " + quoted(chips.speed) + " != " + quoted(speed));
    if (chips.model != model_)
        fail(where + ": model " + quoted(chips.model) + " != " + quoted(model_));
    if (chips.state != state)
        fail(where + ": state " + quoted(chips.state) + " != " + quoted(state));
    // A chip is never empty: the row would show a hole instead of an absence.
    for (const std::string* chip : {&chips.context, &chips.speed, &chips.model, &chips.state})
        if (chip->empty())
            fail(where + ": an empty chip was produced");
}

// A full sentence as D writes it while a run is active, with the model quoting
// D adds and the "Ready" tail.
void scenario1_fullSentenceReady() {
    g_scenario = 1;
    phase("full sentence, Ready");
    expectChips("Context: 42000/128000 tokens | 38.1 tok/s | Model: 'qwen3-coder' | Ready",
                "42k/128k", "38.1 tok/s", "qwen3-coder", "Ready");
}

// The same sentence at the end of a run: only the state differs, so the state chip
// must follow it.
void scenario2_busy() {
    g_scenario = 2;
    phase("full sentence, Busy");
    expectChips("Context: 42000/128000 tokens | 38.1 tok/s | Model: 'qwen3-coder' | Busy",
                "42k/128k", "38.1 tok/s", "qwen3-coder", "Busy");
    // Ready and Busy are the only two spellings D emits; both survive verbatim.
    expectChips("Context: 1/2 tokens | 0.0 tok/s | Model: 'm' | Ready", "1/2", "0.0 tok/s", "m",
                "Ready");
}

// The startup default (and the state of a UI D has not talked to yet) has no tok/s
// field: the speed chip is the absence glyph, not "0.0 tok/s".
void scenario3_missingFields() {
    g_scenario = 3;
    phase("missing tok/s renders —");
    expectChips("Context: 0/0 tokens | Model: none | Ready", "0/0", "—", "none", "Ready");
    // A missing context and a missing model are absences too.
    expectChips("38.1 tok/s | Ready", "—", "38.1 tok/s", "—", "Ready");
    expectChips("Ready", "—", "—", "—", "Ready");
    // …and the value is never the number zero for a field that is simply absent.
    const model::StatusChips chips = model::splitStatusText("Ready");
    expect(chips.speed != "0" && chips.speed != "0.0 tok/s", "missing tok/s is not rendered as 0");
    expect(chips.context != "0" && chips.context != "0/0",
           "missing context is not rendered as 0/0");
    expect(chips.model != "0", "missing model is not rendered as 0");
}

// A fragment that is not one of the three metrics is the state chip, verbatim — the
// design's "never dropped, never invented" rule. A recognised state word and an
// unrecognised fragment both land there, in source order.
void scenario4_malformedFragments() {
    g_scenario = 4;
    phase("malformed fragments become the state chip verbatim");

    // A malformed context field cannot fill the context chip, so the fragment stays
    // visible in the state chip and the field renders as absent.
    expectChips("Context: not-a-number tokens | Model: none | Busy", "—", "—", "none",
                "Context: not-a-number tokens | Busy");
    // A state word the design does not list is still the state (a phrase, not a number).
    expectChips("Context: 10/20 tokens | 1.0 tok/s | Model: 'm' | Streaming over SSE", "10/20",
                "1.0 tok/s", "m", "Streaming over SSE");
    // A trailing separator and a stray fragment lose nothing.
    expectChips("Context: 10/20 tokens | Model: 'm' | Ready | ", "10/20", "—", "m", "Ready");
    // "Contextual" is not the context field (no prefix match without a boundary).
    expectChips("Contextual: 1/2 | Ready", "—", "—", "—", "Contextual: 1/2 | Ready");
    // A second context field is not silently overwritten: the first wins, the second
    // is carried verbatim.
    expectChips("Context: 1/2 tokens | Context: 3/4 tokens | Ready", "1/2", "—", "—",
                "Context: 3/4 tokens | Ready");
}

// No sentence at all: every chip is the absence glyph.
void scenario5_emptyInput() {
    g_scenario = 5;
    phase("empty input");
    expectChips("", "—", "—", "—", "—");
    expectChips("   ", "—", "—", "—", "—");
    expectChips(" | ", "—", "—", "—", "—");
}

// A quoted model name may contain the field separator; splitting must not truncate it
// and must not leave the rest of the name in the state chip.
void scenario6_separatorInsideModel() {
    g_scenario = 6;
    phase("' | ' inside a quoted model name");
    expectChips("Context: 100/200 tokens | 1.5 tok/s | Model: 'a | b' | Ready", "100/200",
                "1.5 tok/s", "a | b", "Ready");
    expectChips("Context: 100/200 tokens | Model: 'a | b | c' | Busy", "100/200", "—", "a | b | c",
                "Busy");
    // An unterminated quote keeps what the sentence carried and does not invent a
    // closing quote.
    expectChips("Model: 'half", "—", "—", "half", "—");
    // An empty quoted name is an absence, not an empty chip.
    expectChips("Model: '' | Ready", "—", "—", "—", "Ready");
}

// The sentence D actually sends (`app_agent/ui.d:422`) has no colon after "Context";
// the startup default has one. Both are the same field.
void scenario7_producersFormat() {
    g_scenario = 7;
    phase("D's colon-less format and the startup default are one grammar");
    expectChips("Context 41/8192 tokens | 0.0 tok/s | Model: 'mock' | Ready", "41/8.2k",
                "0.0 tok/s", "mock", "Ready");
    expectChips("Context: 41/8192 tokens | 0.0 tok/s | Model: 'mock' | Ready", "41/8.2k",
                "0.0 tok/s", "mock", "Ready");
    // Extra spaces around fields are tolerated (trim, not a rewrite).
    expectChips(
        "  Context:   42000/128000  tokens   |   38.1 tok/s   |  Model:  'qwen3-coder'  |  Ready ",
        "42k/128k", "38.1 tok/s", "qwen3-coder", "Ready");
}

// The number format itself: the chips are canonical for the numbers (task 27's meter
// shows the same used/max), so the abbreviation is part of the contract.
void scenario8_countFormat() {
    g_scenario = 8;
    phase("used/max abbreviation");
    expectChips("Context: 0/0 tokens", "0/0", "—", "—", "—");
    expectChips("Context: 999/1000 tokens", "999/1k", "—", "—", "—");
    expectChips("Context: 1234/8192 tokens", "1.2k/8.2k", "—", "—", "—");
    expectChips("Context: 42000/128000 tokens", "42k/128k", "—", "—", "—");
    expectChips("Context: 1/2000000 tokens", "1/2000k", "—", "—", "—");
}

// The acceptance criterion in one place: the default sentence's four values, and the
// constant the UI seeds itself with.
void scenario9_defaultSentence() {
    g_scenario = 9;
    phase("default sentence -> 0/0, —, none, Ready");
    expectChips(model::kDefaultStatusSentence, "0/0", "—", "none", "Ready");
    expect(std::string(model::kMissingChipValue) == "—",
           "the absence glyph is the em dash the design documents");
}

// The absence rule has a shape the field parsers used to get wrong: a field that is
// present but *empty* (`Context: 1/ tokens`) parses as "" and must be rejected, not
// read as 0. The same closed grammar keeps strtod's wider vocabulary ("inf", "nan",
// hex floats, exponents) off the row, and the model field stops at its closing quote
// instead of folding the rest of the sentence into the name.
void scenario10_absentNumbersAndClosedGrammar() {
    g_scenario = 10;
    phase("empty numeric fields, closed number grammar, model closing quote");

    // Empty numbers are an absence, not a zero — the one rule the module promises.
    expectChips("Context: 1/ tokens", "—", "—", "—", "Context: 1/ tokens");
    expectChips("Context: / tokens", "—", "—", "—", "Context: / tokens");
    expectChips("Context: /", "—", "—", "—", "Context: /");
    expectChips("Context: 1/2 tokens | tok/s | Model: 'm'", "1/2", "—", "m", "tok/s");

    // strtod's grammar is wider than the chip vocabulary: none of these is a rate.
    expectChips("inf tok/s | Ready", "—", "—", "—", "inf tok/s | Ready");
    expectChips("nan tok/s", "—", "—", "—", "nan tok/s");
    expectChips("0x1p3 tok/s", "—", "—", "—", "0x1p3 tok/s");
    expectChips("1e5 tok/s", "—", "—", "—", "1e5 tok/s");
    expectChips("+5 tok/s", "—", "—", "—", "+5 tok/s");
    // The decimal forms D writes still parse (and only the first one fills the chip).
    expectChips("38.1 tok/s | 0.0 tok/s | 112 tok/s", "—", "38.1 tok/s", "—",
                "0.0 tok/s | 112 tok/s");

    // The name ends at the closing quote; the remainder is a state fragment.
    expectChips("Model: 'a' trailing | Ready", "—", "—", "a", "trailing | Ready");
    expectChips("Model: 'a | b' | Ready", "—", "—", "a | b", "Ready");
}

} // namespace

int main() {
    scenario1_fullSentenceReady();
    scenario2_busy();
    scenario3_missingFields();
    scenario4_malformedFragments();
    scenario5_emptyInput();
    scenario6_separatorInsideModel();
    scenario7_producersFormat();
    scenario8_countFormat();
    scenario9_defaultSentence();
    scenario10_absentNumbersAndClosedGrammar();
    // Nothing in the splitter may look at LC_NUMERIC. The app adopts the user's
    // locale (`tui_api.cpp` setlocale(LC_ALL, "")), and a comma-decimal locale
    // used to turn `42k` into `42,0k` and reject `38.1` through strtod/snprintf.
    // The image only ships C/POSIX/C.utf8, so this is a no-op where the locale is
    // missing and a real check on a desktop that has it.
    if (std::setlocale(LC_NUMERIC, "de_DE.UTF-8") != nullptr) {
        scenario1_fullSentenceReady();
        scenario8_countFormat();
        std::printf("OK: chips are locale-independent (de_DE.UTF-8)\n");
        std::setlocale(LC_NUMERIC, "C");
    }
    std::printf("OK: status-chip splitter scenarios passed\n");
    return 0;
}
