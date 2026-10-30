/// The ABI's private handle shape, for the tests that read back what a `tui*` call wrote.
///
/// `tui_api.h` only forward-declares `typedef struct TuiState TuiState;`, and `tui_api.cpp`
/// completes it with exactly one inner pointer. Completing it the same way here is legal C++ (a
/// forward declaration may be completed once, identically, in every TU) and gives a test the
/// white-box observation point it needs: the C surface is what D links against, so a contract test
/// that only saw the opaque handle could not check that a setter landed.
///
/// One copy, deliberately: several test TUs would otherwise each spell the layout, and a change to
/// the handle would become a silent ODR hazard instead of a compile error.
///
/// No includes: the inner type only ever appears behind a pointer here, so a forward declaration is
/// enough and this header stays usable from a TU that does not have `cpp_tui/` on its include path.
#pragma once

namespace llmfun::tui {
struct TuiState;
}

struct TuiState {
    ::llmfun::tui::TuiState* inner;
};
