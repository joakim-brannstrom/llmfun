## Task 6a: Scenes first - the target rows as failing checks

Priority: P0. Dependencies: Task 6.

### Description
State the target shape as assertions over task_06's four fixtures. Every item is **red** at the end
of this task and names the task that turns it green; tasks 07-18 drive them green one by one. This
is the acceptance surface B5 asked for, without a hand-authored golden.

### Changes Made
- `cpp_tui/test_blocks.cpp` (new `rowOfSub(const Grid&, const char*)` next to `rowOf`).
- `cpp_tui/test_blocks.cpp` (new `checkOwnerShape()`, called from `main()` before the golden
  capture).

```cpp
// The target rows, as substrings. A cell-exact screen is the golden's job; this block is the
// *shape* review - it reads the drawn grid, so it cannot pass on a descriptor alone.
int rowOfSub(const Grid& g, const std::string& needle) {
    for (int y = 0; y < g.ny; ++y)
        if (g.rowText(y).find(needle) != std::string::npos)
            return y;
    return -1;
}

void checkOwnerShape() {
    {   // maxlong (T10, T13): a folded group draws its header, no owner, no card, then the answer
        ThreadFixture f; buildMaxLong(f);
        const Grid g = f.draw();
        check(g.contains("WORK - 30 steps"), "maxlong: the folded header announces 30 steps");
        check(!g.contains("AGENT"), "maxlong: a closed group draws no owner row");
        check(!g.contains("TOOL"), "maxlong: a closed group draws no card");
        check(g.contains("ANSWER") && g.contains("the refactor is done: ..."),
              "maxlong: the answer closes the turn");
    }
    {   // maxlonglive (T7, T10, T11, T13)
        ThreadFixture f; buildMaxLongLive(f);
        const Grid g = f.draw();
        check(g.contains("> 1.1 s - [bash] [grep]"),
              "maxlonglive: a settled owner folds with its duration and chips");
        check(g.contains("v AGENT - [execute_command]"),
              "maxlonglive: the live owner draws open, no duration yet");
        check(g.contains("v Thinking"), "maxlonglive: the live batch's reasoning draws");
        check(g.contains("Next I will move the loader out of core."),
              "maxlonglive: its content draws");
        check(g.contains("TOOL  execute_command"), "maxlonglive: its live card draws");
    }
    {   // maxnotice (T8): the batch block first, the interleaved notice after it
        ThreadFixture f; buildMaxNotice(f);
        const Grid g = f.draw();
        const int owner = rowOfSub(g, "v AGENT");
        const int content = rowOfSub(g, "now the count");
        const int notice = rowOfSub(g, "context compacted");
        check(owner >= 0, "maxnotice: the live owner row is drawn open");
        check(owner >= 0 && content > owner, "maxnotice: its content draws under it");
        check(content >= 0 && notice > content,
              "maxnotice: the notice is emitted after the batch block");
    }
    {   // maxchips (T9, T13)
        ThreadFixture f; buildMaxChips(f);
        const Grid g = f.draw();
        check(g.contains("> 1.2 s - [bash] [grep] [read_file] +1"),
              "maxchips: chips drop to three and a +N");
    }
}
```

### Verification
- [ ] `cd llmfun/build/tui && make test_blocks > build.log 2>&1` clean.
- [ ] `./test_blocks` reports exactly these failures and nothing else; count them and put the
      number in the task report.
- [ ] Every failure names its finalizing task in the check text's neighbouring comment (kept).
- [ ] The four target row sets are quoted in the task report for the human review **before** the
      implementation work starts (design §10.3's first review).
- [ ] No golden changed by this task (`git status` shows the source only).
- [ ] Regression: the overview's regression command passes: the red items are `test_blocks`
      *checks*, not a golden diff, so the suite is green everywhere else.

### Notes
- This task is the plan's one intentional red boundary (see the overview's Standing Executor
  Rules). It exists so that the shape is reviewed before the code, which is B5's intent.
- `maxlonglive`'s `1.1 s` is batch 0's content duration and its chips are `[bash] [grep]`; if the
  chips order or the duration source is wrong, this is the check that says so.
- `maxnotice`'s ordering checks all carry `>= 0` guards: `rowOfSub` answers -1 for a missing row,
  so a bare `-1 < 5` would pass a broken case.
- Do not weaken a check to make it pass: a red check here is a finding for the task that owns it.

Plan pointers:
- B5, §10.3; task_06 holds the four builders it uses.
- Anchors were verified at plan time; re-verify before each edit.
