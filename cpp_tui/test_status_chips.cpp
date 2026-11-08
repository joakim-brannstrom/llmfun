// test_status_chips.cpp
//
// Headless unit tests for the status-chip formatter: the structured status D
// sends every tick (`tuiSetRuntimeStatus`) becomes the four chips the status row
// shows.
//
// What is pinned here, case by case:
//   - the documented numbers map to the documented chips (`42k/128k`, `38.1 tok/s`,
//     `qwen3-coder`, `Ready`) and `Busy` is a state like `Ready`;
//   - a number the status does not carry (`contextMax == 0`, `tokensPerSecond == 0`,
//     an empty `model`) renders "—", never "0" (a number and its absence must stay
//     distinguishable — the composer's mini meter shows the same numbers);
//   - the seven phases name the four state words, and an unknown phase ordinal
//     answers `Idle`, never a neighbouring phase;
//   - the numbers the context chip was formatted from are carried back on the
//     struct (`contextUsed`/`contextMax`), so a meter never parses the chip;
//   - the seed state a `tuiCreateState` build carries (`model == "none"`, zeroed
//     numbers) yields exactly `—`, `—`, `none`, `Ready`.
//
// Layering check, by construction: this binary compiles model/status_chips.cpp directly
// and links nothing else, so the module cannot have grown an ImGui/tui.h dependency.
//
// Exit codes: 0 = all scenarios pass; 1 = first assertion failure.
// Run from the build dir: ./test_status_chips

#include "model/status_chips.h"

#include <clocale>
#include <cstdint>
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

/// The full status D sends while a run is idle: the documented chip numbers.
model::RuntimeStatus fullStatus() {
    model::RuntimeStatus status;
    status.phase = model::phaseOrdinal(model::Phase::Idle);
    status.model = "qwen3-coder";
    status.contextUsed = 42000;
    status.contextMax = 128000;
    status.compactAt = 115200;
    status.tokensPerSecond = 38.1;
    return status;
}

/// Asserts all four chips of one status and returns them for follow-up asserts.
model::StatusChips expectChips(const model::RuntimeStatus& status, const std::string& context,
                               const std::string& speed, const std::string& model_,
                               const std::string& state) {
    const model::StatusChips chips = model::chipsFromStatus(status);
    const std::string where =
        "status (phase " + std::to_string(status.phase) + ", model " + quoted(status.model) + ")";
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
    return chips;
}

// The full status maps to the documented chips, and only the state word changes
// when the run ends.
void scenario1_fullStatus() {
    g_scenario = 1;
    phase("full status, Ready and Busy");
    expectChips(fullStatus(), "42k/128k", "38.1 tok/s", "qwen3-coder", "Ready");

    model::RuntimeStatus busy = fullStatus();
    busy.phase = model::phaseOrdinal(model::Phase::Writing);
    expectChips(busy, "42k/128k", "38.1 tok/s", "qwen3-coder", "Busy");
}

// A number the status does not carry is the absence glyph, never a zero: no
// capacity (`contextMax == 0`), no rate yet (`tokensPerSecond == 0`), no name
// reported (an empty `model`).
void scenario2_absences() {
    g_scenario = 2;
    phase("missing numbers render —");

    const model::RuntimeStatus bare; // zeroed numbers, no model
    const model::StatusChips chips = expectChips(bare, "—", "—", "—", "Ready");
    expect(chips.speed != "0" && chips.speed != "0.0 tok/s", "missing tok/s is not rendered as 0");
    expect(chips.context != "0" && chips.context != "0/0",
           "missing context is not rendered as 0/0");
    expect(chips.model != "0", "missing model is not rendered as 0");
    // The absent pair leaves the meter numbers at zero: a meter must not divide
    // by an absence.
    expect(chips.contextUsed == 0 && chips.contextMax == 0,
           "an absent pair carries no meter numbers");
}

// The number format itself: the chips are canonical for the numbers (the
// composer's meter shows the same used/max), so the abbreviation is part of the
// contract. The numbers are carried back on the struct.
void scenario3_countFormat() {
    g_scenario = 3;
    phase("used/max abbreviation");

    const auto expectContext = [](std::int64_t used, std::int64_t max, const std::string& want) {
        model::RuntimeStatus status;
        status.contextUsed = used;
        status.contextMax = max;
        const model::StatusChips chips = model::chipsFromStatus(status);
        if (chips.context != want)
            fail("context " + quoted(chips.context) + " != " + quoted(want));
        if (max > 0)
            expect(chips.contextUsed == used && chips.contextMax == max,
                   "the meter numbers are carried back on the chips");
        else
            expect(chips.contextUsed == 0 && chips.contextMax == 0,
                   "an absent pair carries no meter numbers");
    };
    expectContext(999, 1000, "999/1k");
    expectContext(1234, 8192, "1.2k/8.2k");
    expectContext(42000, 128000, "42k/128k");
    expectContext(1, 2000000, "1/2000k");
    // A used count above zero with no capacity is still an absence.
    expectContext(41, 0, "—");
    expectContext(0, 0, "—");
}

// The seven phases name the four state words, and a phase this build has no member
// for answers `Idle` (`Ready`), never a neighbouring phase.
void scenario4_stateWords() {
    g_scenario = 4;
    phase("phase -> state word");

    model::RuntimeStatus status;
    const auto expectState = [&](const char* want) {
        const model::StatusChips chips = model::chipsFromStatus(status);
        if (chips.state != want)
            fail("phase " + std::to_string(status.phase) + ": state " + quoted(chips.state) +
                 " != " + quoted(want));
    };
    status.phase = model::phaseOrdinal(model::Phase::Idle);
    expectState("Ready");
    status.phase = model::phaseOrdinal(model::Phase::Thinking);
    expectState("Busy");
    status.phase = model::phaseOrdinal(model::Phase::Tooling);
    expectState("Busy");
    status.phase = model::phaseOrdinal(model::Phase::Writing);
    expectState("Busy");
    status.phase = model::phaseOrdinal(model::Phase::Waiting);
    expectState("Busy");
    status.phase = model::phaseOrdinal(model::Phase::Error);
    expectState("Error");
    status.phase = model::phaseOrdinal(model::Phase::Offline);
    expectState("Offline");
    // The ordinals themselves are the ABI's numbers (`TuiRuntimeStatus.phase`): D's
    // `TuiPhase` pins this same table (llm/tui/tests.d), so reordering either enum
    // breaks the other side's pin. `phaseOrdinal` above would silently follow a
    // reorder; these literals cannot.
    expect(model::phaseOrdinal(model::Phase::Idle) == 0 &&
               model::phaseOrdinal(model::Phase::Thinking) == 1 &&
               model::phaseOrdinal(model::Phase::Tooling) == 2 &&
               model::phaseOrdinal(model::Phase::Writing) == 3 &&
               model::phaseOrdinal(model::Phase::Waiting) == 4 &&
               model::phaseOrdinal(model::Phase::Error) == 5 &&
               model::phaseOrdinal(model::Phase::Offline) == 6,
           "the phase ordinals are the ABI table D's TuiPhase pins");
    // A newer sender's ordinal answers idle: the row says "Ready", not a phase
    // it cannot name.
    status.phase = 7;
    expectState("Ready");
    status.phase = -1;
    expectState("Ready");
}

// The seed state a `tuiCreateState` build carries: the model chip filled with the
// name D itself reports for "no model", everything else absent.
void scenario5_seed() {
    g_scenario = 5;
    phase("seed status -> —, —, none, Ready");
    model::RuntimeStatus seed;
    seed.model = "none";
    expectChips(seed, "—", "—", "none", "Ready");
    expect(std::string(model::kMissingChipValue) == "—",
           "the absence glyph is the em dash the design documents");
}

// Equality is field-deep: the frame layer's "did the chip source change" test
// compares instead of re-deriving.
void scenario6_equality() {
    g_scenario = 6;
    phase("operator== / !=");
    model::StatusChips a;
    a.context = "42k/128k";
    a.speed = "38.1 tok/s";
    a.model = "qwen3-coder";
    a.state = "Ready";
    a.contextUsed = 42000;
    a.contextMax = 128000;

    model::StatusChips b = a;
    expect(a == b, "copies are equal");
    b.speed = "37.9 tok/s";
    expect(a != b, "a differing chip breaks equality");
    b = a;
    b.contextMax = 0;
    expect(a != b, "a differing meter number breaks equality");
}

} // namespace

int main() {
    scenario1_fullStatus();
    scenario2_absences();
    scenario3_countFormat();
    scenario4_stateWords();
    scenario5_seed();
    scenario6_equality();
    // Nothing in the formatter may look at LC_NUMERIC. The app adopts the user's
    // locale (`tui_api.cpp` setlocale(LC_ALL, "")), and a comma-decimal locale
    // would otherwise turn `42k` into `42,0k` through snprintf. The image only
    // ships C/POSIX/C.utf8, so this is a no-op there and a real check on a
    // desktop that has the locale.
    if (std::setlocale(LC_NUMERIC, "de_DE.UTF-8") != nullptr) {
        scenario1_fullStatus();
        scenario3_countFormat();
        std::printf("OK: chips are locale-independent (de_DE.UTF-8)\n");
        std::setlocale(LC_NUMERIC, "C");
    }
    std::printf("OK: %d scenarios\n", g_scenario);
    return 0;
}
