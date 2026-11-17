## Task 19: Scene review and the new checks

Priority: P1. Dependencies: Tasks 10-18.

### Description
Look at the capture and lock the behaviour: a human reviews the four new scenes, every check in
design §10.3 exists as an assertion, and no golden changed without a one-line reason.

### Changes Made
- `cpp_tui/test_blocks.cpp` (edit): the §10.3 checks that are screens (grouped below); the
  `checkOwnerShape()` block from task_06a loses its "red" comments.
- `cpp_tui/test_typed_thread_store.cpp` (edit): the projection-level checks that need no screen.
- `cpp_tui/test/golden/*` (regolden only what the review moves, with the reason).

```cpp
// §10.3's check list, verbatim - each item becomes a check() whose text is the quoted string.
// A: the fold and the owner
//   "the owner header carries chips, duration and a disclosure mark"
//   "a live batch draws open with its thinking, content and cards"
//   "the last call folds it to '> 3.2 s - [chips]', with no role word"
//   "a reasoning-less batch folds the same way"
//   "a content-only batch previews its first line"
//   "a failed call marks the folded header"
//   "an owner with no surviving child is absent"
// B: minimal
//   "minimal keeps the live content row and draws no owner row"
//   "the content row disappears when the turn closes"
//   "neither the shadow line nor the chips line ever draws in maximal"
// C: displayability and steps
//   "a hidden block weighs 0 and draws nothing"
//   "the query batch never grows an owner"
//   "stepCount with hiddenTools = [] is unchanged; with the default it is -2 per hidden call"
// D: identity and pins
//   "no row id repeats within a turn"
//   "a pin survives the default flipping"
//   "a re-opened settled batch shows its content with thinking and cards folded"
```

### Verification
- [ ] `make test_blocks test_typed_thread_store test_streaming > build.log 2>&1` clean.
- [ ] Group A above: all seven assertions exist and pass.
- [ ] Group B above: all three exist and pass.
- [ ] Group C above: all three exist and pass.
- [ ] Group D above: all three exist and pass.
- [ ] `./test_blocks` prints its full check count green; the four new scenes' captures are
      reviewed by a human and the verdict is in the task report.
- [ ] `git diff --stat test/golden` lists only files whose task claimed them, each with a reason.
- [ ] Regression: the overview's regression command passes.

### Notes
- This is the *second* human review (the first is task_06a's target rows). If a capture reads badly,
  the finding is a re-plan request, not a `--update`: never regolden a shape you dislike.
- The `+N` chips and the notice placement are the two shapes most likely to need a tweak; both are
  single-line changes in `chipsText` (task_07) and the emission order (task_08).

Plan pointers:
- §10.1, §10.3, NFR-1, NFR-6; overview open issue 1 (chips art).
- Anchors were verified at plan time; re-verify before each edit.
