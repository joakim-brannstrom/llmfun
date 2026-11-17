## Task 6: Scenes first - the four new scenes and their captures

Priority: P0. Dependencies: Task 5.

### Description
Put the new shape on the board before the projection work: four builders (`maxlong`,
`maxlonglive`, `maxnotice`, `maxchips`), four scenes and their committed golden captures. The
*target rows* are task_06a's; this task is the data and the capture.

### Changes Made
- `cpp_tui/test_blocks.cpp` (new, before `scenes()`): four `buildX(ThreadFixture&)` helpers in
  the anonymous namespace (verbatim below).
- `cpp_tui/test_blocks.cpp:1906-2054` (`scenes()`, edit): four entries calling the builders.
- `cpp_tui/test/golden/blocks-maxlong-100x30.txt`, `blocks-maxlonglive-100x30.txt`,
  `blocks-maxnotice-100x30.txt`, `blocks-maxchips-100x30.txt` (new, via `--update`).

```cpp
// --- maxlong: a finished six-response turn, maximal. 6 content rows (1 step each) + 12
// settled calls (2 each) = 30 steps; the group is folded, so only its header draws.
void buildMaxLong(ThreadFixture& f) {
    f.state.chat.threadMinimalMode = false;
    f.seed.upsertTurn(1, TuiTurnState_Done, 12);
    f.seed.upsertBlock(1, "query", "1#q", TuiBlockKind_Prose, TuiBlockStatus_None, "user",
                       "refactor the whole tree, module by module");
    for (int r = 0; r < 6; ++r) {                    // upsertBlock(..., title, durationMs)
        const std::string t = std::to_string(r);
        f.seed.upsertBlock(1, "content#" + t, "1#" + t, TuiBlockKind_Prose, TuiBlockStatus_None,
                           "assistant", "step " + t + " done", nullptr, 1200);
        f.seed.upsertBlock(1, "call#a" + t, "1#" + t, TuiBlockKind_Tool, TuiBlockStatus_Ok, "ok",
                           "ls -R", "bash", 1100);
        f.seed.upsertBlock(1, "call#b" + t, "1#" + t, TuiBlockKind_Tool, TuiBlockStatus_Ok, "ok",
                           "grep -r needle", "grep", 1200);
    }
    f.seed.upsertBlock(1, "answer", "1#5", TuiBlockKind_Answer, TuiBlockStatus_None, nullptr,
                       "the refactor is done: ...");
}

// --- maxlonglive: the same turn mid-run. Batches 0 and 1 settled, batch 2 live (owner open,
// thinking + content + one running card).
void buildMaxLongLive(ThreadFixture& f) {
    f.state.chat.threadMinimalMode = false;
    f.seed.runStart("refactor the whole tree, module by module");
    for (int r = 0; r < 2; ++r) {
        const std::string t = std::to_string(r);
        f.seed.upsertBlock(1, "content#" + t, "1#" + t, TuiBlockKind_Prose, TuiBlockStatus_None,
                           "assistant", "step " + t + " done", nullptr, 1100 + 100 * r);
        f.seed.upsertBlock(1, "call#a" + t, "1#" + t, TuiBlockKind_Tool, TuiBlockStatus_Ok, "ok",
                           "ls -R", "bash", 1100);
        f.seed.upsertBlock(1, "call#b" + t, "1#" + t, TuiBlockKind_Tool, TuiBlockStatus_Ok, "ok",
                           "grep -r needle", "grep", 1200);
        f.seed.nextBatch();
    }
    f.seed.upsertBlock(1, "reasoning#0", "1#2", TuiBlockKind_Reasoning, TuiBlockStatus_None,
                       nullptr, "rebasing the parser onto the new loader ...");
    f.seed.upsertBlock(1, "content#0", "1#2", TuiBlockKind_Prose, TuiBlockStatus_None,
                       "assistant", "Next I will move the loader out of core.");
    f.seed.upsertBlock(1, "call#0", "1#2", TuiBlockKind_Tool, TuiBlockStatus_Running, nullptr,
                       "dub test", "execute_command", 0);
}

// --- maxnotice: a LIVE turn, two batches. Batch 0 settled (its owner folds), batch 1 live (its
// owner is open) and the notice sits BETWEEN two rows of batch 1.
void buildMaxNotice(ThreadFixture& f) {
    f.state.chat.threadMinimalMode = false;
    f.seed.runStart("sweep the tree");
    f.seed.upsertBlock(1, "content#0", "1#0", TuiBlockKind_Prose, TuiBlockStatus_None,
                       "assistant", "I will list the tree first.", nullptr, 1100);
    f.seed.upsertBlock(1, "call#0", "1#0", TuiBlockKind_Tool, TuiBlockStatus_Ok, "42 lines",
                       "ls -R", "bash", 1100);
    f.seed.nextBatch();
    f.seed.upsertBlock(1, "content#1", "1#1", TuiBlockKind_Prose, TuiBlockStatus_None,
                       "assistant", "now the count");
    f.seed.upsertBlock(1, "notice#1", "1#q", TuiBlockKind_Notice, TuiBlockStatus_None, nullptr,
                       "context compacted");
    f.seed.upsertBlock(1, "call#1", "1#1", TuiBlockKind_Tool, TuiBlockStatus_Running, nullptr,
                       "wc -l", "bash", 0);
}

// --- maxchips: a settled batch of four distinct tools -> "[a] [b] [c] +1", duration from the
// response's content row (the owner carries it, DD-8).
void buildMaxChips(ThreadFixture& f) {
    f.state.chat.threadMinimalMode = false;
    f.seed.upsertTurn(1, TuiTurnState_Done, 9);
    f.seed.upsertBlock(1, "query", "1#q", TuiBlockKind_Prose, TuiBlockStatus_None, "user", "go");
    f.seed.upsertBlock(1, "content#0", "1#0", TuiBlockKind_Prose, TuiBlockStatus_None,
                       "assistant", "doing four things", nullptr, 1200);
    const char* names[] = {"bash", "grep", "read_file", "write_file"};
    for (int i = 0; i < 4; ++i)
        f.seed.upsertBlock(1, "call#" + std::to_string(i), "1#0", TuiBlockKind_Tool,
                           TuiBlockStatus_Ok, "ok", names[i], names[i], 1200);
}
```

```cpp
// scenes() gains four rows (the builders above):
list.push_back({"maxlong", &buildMaxLong});
list.push_back({"maxlonglive", &buildMaxLongLive});
list.push_back({"maxnotice", &buildMaxNotice});
list.push_back({"maxchips", &buildMaxChips});
```

### Verification
- [ ] `cd llmfun/build/tui && make test_blocks > build.log 2>&1` clean.
- [ ] The four scenes exist and draw (no assertion yet); `./test_blocks` is fully green.
- [ ] The four golden files are committed, created with `--update` (today's screen - they become
      the review surface in task_19).
- [ ] `./test_streaming`, `./test_thread_window`, `./test_typed_thread_store` unchanged.
- [ ] Regression: the overview's regression command passes.

### Notes
- **Deviation from design B5, recorded.** B5 asks for the new scenes committed as *failing
  goldens*. A byte-exact target screen cannot be authored before the layout exists, so this plan
  keeps every golden green and puts the target shape into `task_06a.md` as failing **checks**.
  Same contract, human-reviewable, and NFR-10 stays intact. If the user prefers literal red
  goldens, hand-edit the four `text:` sections and accept a red suite until task_15.
- The owner's duration comes from its batch's **content** row (`durationMs`), read before any
  drop (DD-8) - hence the `nullptr, 1200` arguments. A batch with no content row has no duration.
- `maxnotice` is a live turn on purpose: a settled owner folds to `> <duration> - [chips]` and
  would not read `v AGENT`, and an opened batch is where the placement rule is observable.
- `maxchips` needs four *distinct* names: four calls of three names print no `+N`.
- Keep every scene at 100x30: the harness is fixed (`test_blocks.cpp:123-124`).

Plan pointers:
- B5, §10.3 ("new scenes"), NFR-1/NFR-10; overview open issues 1 and 3.
- task_06a states the target rows these scenes must reach.
- Anchors were verified at plan time; re-verify before each edit.
