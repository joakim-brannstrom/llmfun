// test_tui_fontchain.cpp - headless seam test for the GUI font chain C API
// (tuiSetFontChain + tuiFontChainCount). No backend, no rendering: the test
// drives plain handles. The setter is tolerant by design (no cap, no validation):
// storage is byte-exact across the D->C boundary and load failures surface only
// when the backend builds the atlas, so the seam proves storage + read-back.

#include "tui_api.h" // C API: tuiCreateState/tuiDestroyState/tuiSetFontChain/tuiFontChainCount

#include <cstdio>
#include <string>
#include <vector>

// `TuiState`: the handle's private shape, shared by the contract tests that
// read back what a `tui*` call wrote (see test/tui_state_handle.h).
#include "test/tui_state_handle.h"
// The inner state type must be complete for the white-box read-back
// (inner->fontChain) - same include tui_state_handle.h's users pair it with.
#include "tui.h"

namespace {

int g_case = 0;
const char* g_caseName = "";

void fail(const std::string& what) {
    std::fprintf(stderr, "FAIL [case %d %s]: %s\n", g_case, g_caseName, what.c_str());
    std::exit(1);
}

void expect(bool cond, const std::string& what) {
    if (!cond)
        fail(what);
}

// The String entries are borrowed for the call (tui_api.h String convention).
// Case A builds its entries from REAL stack buffers that die (and are poisoned)
// before the read-back, so only a copy can pass; the helper below wraps string
// literals for cases D/E, which prove stored content only - literals have
// static storage duration, so they can never prove the copy.
String lit(const char* s) { return String{s, std::string(s).size()}; }

void expectChain(::TuiState* st, const std::vector<std::string>& want, const std::string& what) {
    expect(st != nullptr && st->inner != nullptr, what + ": state alive");
    const size_t n = tuiFontChainCount(st);
    if (n != want.size()) {
        fail(what + ": tuiFontChainCount is " + std::to_string(n) + " (want " +
             std::to_string(want.size()) + ")");
    }
    const std::vector<std::string>& got = st->inner->fontChain;
    if (got != want) {
        std::string dump = what + ": fontChain = [";
        for (const auto& s : got)
            dump += "'" + s + "',";
        dump += "]";
        fail(dump);
    }
}

} // namespace

int main() {
    // Case A: the D->C round trip - two absolute paths in fallback order,
    // built from REAL stack buffers that are garbage-filled and then die
    // before the assertion: if the setter stored the pointers instead of
    // copying, the poisoned read-back below would show garbage.
    g_case = 1;
    g_caseName = "round trip";
    ::TuiState* st = tuiCreateState();
    expect(st != nullptr && st->inner != nullptr, "case A: tuiCreateState succeeds");
    {
        char aBuf[] = "a.ttf";
        char bBuf[] = "b.ttf";
        String arr[2] = {String{aBuf, 5}, String{bBuf, 5}};
        tuiSetFontChain(st, arr, 2);
        // Poison the borrowed buffers while they are still alive: the setter
        // must have copied during the call, so the poison must not reach
        // the read-back (a pointer-storing setter would fail here).
        for (char& c : aBuf)
            c = '\xFA';
        for (char& c : bBuf)
            c = '\xFA';
    } // aBuf/bBuf really die here
    expectChain(st, {"a.ttf", "b.ttf"}, "case A");

    // Case B: an empty list (NULL paths, count 0) clears the chain = the
    // builtin behavior. Runs on the same state, so it also proves clearing
    // a non-empty chain.
    g_case = 2;
    g_caseName = "clear";
    tuiSetFontChain(st, nullptr, 0);
    expectChain(st, {}, "case B: NULL/0 clears");

    // Case B2: NULL paths with a NONZERO count is also a clear - the setter's
    // guard treats a NULL list as empty instead of dereferencing it (the
    // header contract says "count == 0 or NULL paths" clears).
    tuiSetFontChain(st, nullptr, 2);
    expectChain(st, {}, "case B2: NULL paths with nonzero count clears");

    // Case C: a NULL String entry inside the list is tolerated and stored
    // as an empty string; the count still reports every slot (per-entry
    // failures are load-time skips later, not storage-time errors).
    g_case = 3;
    g_caseName = "null entry";
    String mixed[3] = {String{"c.ttf", 5}, String{nullptr, 0}, String{"e.ttf", 5}};
    tuiSetFontChain(st, mixed, 3);
    expectChain(st, {"c.ttf", "", "e.ttf"}, "case C");

    // Case D: setting again overwrites wholesale (the ctor calls this once
    // per process, but the contract is "last set wins").
    g_case = 4;
    g_caseName = "overwrite";
    String one[1] = {lit("only.ttf")};
    tuiSetFontChain(st, one, 1);
    expectChain(st, {"only.ttf"}, "case D: overwritten to 1 entry");

    tuiDestroyState(st);

    // Case E: null state is null-safe everywhere.
    g_case = 5;
    g_caseName = "null state";
    String arr2[2] = {lit("x.ttf"), lit("y.ttf")};
    tuiSetFontChain(nullptr, arr2, 2); // must not crash
    expect(tuiFontChainCount(nullptr) == 0, "case E: tuiFontChainCount(NULL) is 0");

    std::printf("OK: all font-chain seam cases passed\n");
    return 0;
}
